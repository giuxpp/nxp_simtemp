#include <QtWidgets>
#include <QtCharts>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utility>

using namespace QtCharts;

#pragma pack(push,1)
struct simtemp_sample {
    uint64_t timestamp_ns;
    int32_t  temp_mC;
    uint32_t flags; // bit0=NEW_SAMPLE, bit1=THRESHOLD_CROSSED
};
#pragma pack(pop)

static const char* DEV_PATH = "/dev/simtemp";
static const char* SYSFS_BASE = "/sys/class/misc/simtemp";
static const char* ALERT_GREEN = "#2e7d32";
static const char* ALERT_RED   = "#c62828";

class SimtempGui : public QWidget {
    Q_OBJECT
public:
    SimtempGui(QWidget* parent=nullptr)
        : QWidget(parent),
          fd_(-1),
          notifier_(nullptr),
          series_(new QLineSeries(this)),
          chart_(new QChart()),
          view_(new QChartView(chart_)),
          alertLamp_(new QLabel("●")),
          thresholdSpin_(new QSpinBox()),
          samplingSpin_(new QSpinBox()),
          modeCombo_(new QComboBox()),
          status_(new QLabel),
          resetAlertBtn_(new QPushButton("Reset Alert")),
          statsBtn_(new QPushButton("Print Stats")),
          simToggleBtn_(new QPushButton("Stop Simulation")),
          alertLatched_(false),
          running_(true)
    {
        fd_ = ::open(DEV_PATH, O_RDONLY);
        if (fd_ < 0) {
            QMessageBox::critical(this, "Error", QString("Cannot open %1").arg(DEV_PATH));
            qApp->exit(1);
            return;
        }

        notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
        connect(notifier_, &QSocketNotifier::activated, this, &SimtempGui::onDeviceReadable);

        series_->setUseOpenGL(true);
        chart_->legend()->hide();
        chart_->addSeries(series_);
        auto axisX = new QValueAxis;
        axisX->setTitleText("samples");
        axisX->setRange(0, MAX_POINTS - 1);
        auto axisY = new QValueAxis;
        axisY->setTitleText("°C");
        axisY->setRange(0, 100);
        chart_->addAxis(axisX, Qt::AlignBottom);
        chart_->addAxis(axisY, Qt::AlignLeft);
        series_->attachAxis(axisX);
        series_->attachAxis(axisY);
        view_->setRenderHint(QPainter::Antialiasing);

        samplingSpin_->setRange(1, 10000);
        samplingSpin_->setSuffix(" ms");
        thresholdSpin_->setRange(-50000, 200000);
        thresholdSpin_->setSuffix(" m°C");
        modeCombo_->addItems(QStringList() << "normal" << "noisy" << "ramp");

        alertLamp_->setAlignment(Qt::AlignCenter);
        auto alertText = new QLabel("Alert");

        auto writeBtn = new QPushButton("Apply");
        auto refreshBtn = new QPushButton("Refresh");

        auto form = new QFormLayout;
        form->addRow("sampling_ms:", samplingSpin_);
        form->addRow("threshold_mC:", thresholdSpin_);
        form->addRow("mode:", modeCombo_);

        auto cfgButtons = new QHBoxLayout;
        cfgButtons->addWidget(writeBtn);
        cfgButtons->addWidget(refreshBtn);
        cfgButtons->addStretch();

        auto utilityButtons = new QHBoxLayout;
        utilityButtons->addWidget(resetAlertBtn_);
        utilityButtons->addWidget(statsBtn_);
        utilityButtons->addWidget(simToggleBtn_);
        utilityButtons->addStretch();

        auto lampBox = new QHBoxLayout;
        lampBox->addWidget(alertText);
        lampBox->addWidget(alertLamp_);
        lampBox->addStretch();

        auto side = new QVBoxLayout;
        side->addLayout(form);
        side->addLayout(cfgButtons);
        side->addSpacing(12);
        side->addLayout(lampBox);
        side->addLayout(utilityButtons);
        side->addStretch();
        side->addWidget(status_);

        auto main = new QHBoxLayout(this);
        main->addWidget(view_, 3);
        main->addLayout(side, 1);

        setWindowTitle("nxp_simtemp — Live Monitor");
        resize(950, 540);

        connect(writeBtn, &QPushButton::clicked, this, &SimtempGui::applySysfs);
        connect(refreshBtn, &QPushButton::clicked, this, &SimtempGui::readSysfs);
        connect(resetAlertBtn_, &QPushButton::clicked, this, &SimtempGui::resetAlertLamp);
        connect(statsBtn_, &QPushButton::clicked, this, &SimtempGui::showStats);
        connect(simToggleBtn_, &QPushButton::clicked, this, &SimtempGui::toggleSimulation);

        notifier_->setEnabled(running_);
        resetAlertLamp();
        readSysfs();
        updateSimulationButton();
    }

    ~SimtempGui() override {
        if (fd_ >= 0) ::close(fd_);
    }

private slots:
    void onDeviceReadable() {
        if (!running_)
            return;

        simtemp_sample s{};
        ssize_t n = ::read(fd_, &s, sizeof(s));
        if (n != (ssize_t)sizeof(s)) {
            status_->setText("short read / device error");
            return;
        }

        const double tempC = s.temp_mC / 1000.0;
        const bool alert = (s.flags & (1u << 1)) != 0;

        if (points_.size() >= MAX_POINTS)
            points_.remove(0, 1);
        points_.append(tempC);
        updateSeriesAxis();

        if (alert) {
            alertLatched_ = true;
            setAlertLampColor(ALERT_RED);
        } else if (!alertLatched_) {
            setAlertLampColor(ALERT_GREEN);
        }

        status_->setText(QString("temp=%1°C flags=0x%2")
                         .arg(tempC, 0, 'f', 3)
                         .arg(QString::number(s.flags, 16)));
    }

    void applySysfs() {
        const int sampling = samplingSpin_->value();
        const int thr = thresholdSpin_->value();
        const QString mode = modeCombo_->currentText();

        QStringList errors;
        QString errMsg;
        if (!writeAttr("sampling_ms", QString::number(sampling), &errMsg))
            errors << QString("sampling_ms (%1)").arg(errMsg);
        if (!writeAttr("threshold_mC", QString::number(thr), &errMsg))
            errors << QString("threshold_mC (%1)").arg(errMsg);
        if (!writeAttr("mode", mode, &errMsg))
            errors << QString("mode (%1)").arg(errMsg);

        if (errors.isEmpty()) {
            readSysfs();
            status_->setText("sysfs applied");
        } else {
            status_->setText(QString("sysfs write failed: %1").arg(errors.join(", ")));
        }
    }

    void readSysfs() {
        bool okSampling = false, okThr = false, okMode = false;
        const QString sampling = readAttr("sampling_ms", &okSampling);
        const QString thr = readAttr("threshold_mC", &okThr);
        const QString mode = readAttr("mode", &okMode);

        if (okSampling)
            samplingSpin_->setValue(sampling.toInt());
        if (okThr)
            thresholdSpin_->setValue(thr.toInt());
        if (okMode) {
            int idx = modeCombo_->findText(mode);
            if (idx >= 0)
                modeCombo_->setCurrentIndex(idx);
        }

        status_->setText("sysfs read");
    }

    void resetAlertLamp() {
        alertLatched_ = false;
        setAlertLampColor(ALERT_GREEN);
        status_->setText("alert reset");
    }

    void toggleSimulation() {
        if (!notifier_)
            return;
        running_ = !running_;
        notifier_->setEnabled(running_);
        updateSimulationButton();
        status_->setText(running_ ? "simulation running" : "simulation paused");
    }

    void showStats() {
        bool ok = false;
        const QString stats = readAttr("stats", &ok);
        if (!ok) {
            QMessageBox::warning(this, "Stats", "stats read failed (need sudo?)");
            status_->setText("stats read failed");
            return;
        }

        QMessageBox::information(this, "Stats", stats);
        status_->setText("stats displayed");
    }

private:
    void updateSeriesAxis() {
        QVector<QPointF> pts;
        pts.reserve(points_.size());
        for (int i = 0; i < points_.size(); ++i)
            pts.append(QPointF(i, points_[i]));
        series_->replace(pts);

        if (!points_.isEmpty()) {
            auto [mn, mx] = minmax();
            double pad = qMax(0.5, (mx - mn) * 0.1);
            chart_->axisY()->setRange(mn - pad, mx + pad);
            chart_->axisX()->setRange(0, qMax(50, points_.size()));
        }
    }

    std::pair<double, double> minmax() const {
        double mn = 1e9, mx = -1e9;
        for (double v : points_) {
            mn = qMin(mn, v);
            mx = qMax(mx, v);
        }
        return {mn, mx};
    }

    QString sysPath(const char* name) const {
        return QString("%1/%2").arg(SYSFS_BASE).arg(name);
    }

    QString readAttr(const char* name, bool* ok = nullptr) {
        QFile f(sysPath(name));
        if (!f.open(QIODevice::ReadOnly)) {
            if (ok) *ok = false;
            return {};
        }
        QByteArray data = f.readAll();
        f.close();
        if (ok) *ok = true;
        return QString::fromUtf8(data).trimmed();
    }

    bool writeAttr(const char* name, const QString& val, QString* error = nullptr) {
        QFile f(sysPath(name));
        if (error) error->clear();
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (error) *error = f.errorString();
            return false;
        }
        QByteArray data = val.toUtf8();
        data.append('\n');
        auto n = f.write(data);
        if (!f.flush()) {
            if (error) *error = f.errorString();
            f.close();
            return false;
        }
        f.close();
        if (n != data.size()) {
            if (error) *error = f.errorString();
            return false;
        }
        return true;
    }

    void setAlertLampColor(const QString& color) {
        if (alertLampColor_ == color)
            return;
        alertLamp_->setStyleSheet(QString("font-size:28px;color:%1;").arg(color));
        alertLampColor_ = color;
    }

    void updateSimulationButton() {
        simToggleBtn_->setText(running_ ? "Stop Simulation" : "Start Simulation");
    }

private:
    static constexpr int MAX_POINTS = 512;

    int fd_;
    QSocketNotifier* notifier_;
    QLineSeries* series_;
    QChart* chart_;
    QChartView* view_;
    QLabel* alertLamp_;
    QSpinBox* thresholdSpin_;
    QSpinBox* samplingSpin_;
    QComboBox* modeCombo_;
    QLabel* status_;
    QPushButton* resetAlertBtn_;
    QPushButton* statsBtn_;
    QPushButton* simToggleBtn_;

    bool alertLatched_;
    bool running_;
    QString alertLampColor_;
    QVector<double> points_;
};

int main(int argc, char** argv) {
#ifdef USING_QT5
    QApplication app(argc, argv);
#else
    QApplication app(argc, argv);
#endif
    SimtempGui w;
    w.show();
    return app.exec();
}

#include "main.moc"
