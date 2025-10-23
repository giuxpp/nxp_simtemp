#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kDevPath = "/dev/simtemp";          // character device exposed by the driver
constexpr const char* kSysfsBase = "/sys/class/misc/simtemp"; // sysfs directory for configuration/stats

#pragma pack(push, 1)
struct SimtempSample {
    uint64_t timestamp_ns;
    int32_t temp_mC;
    uint32_t flags;
};
#pragma pack(pop)

struct SimtempStats {
    long long total_samples = 0;
    long long threshold_crossings = 0;
};

std::string SysfsPath(const std::string& attr) {
    return std::string(kSysfsBase) + "/" + attr;
}

bool PathExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open " + path + " for reading");
    }
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
        contents.pop_back();
    }
    return contents;
}

int WriteAttr(const std::string& attr, const std::string& value) {
    const std::string path = SysfsPath(attr);
    // Attributes are small text files: open/write/close is sufficient.
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }

    std::string payload = value;
    if (!payload.empty() && payload.back() != '\n') {
        payload.push_back('\n');
    }

    errno = 0;
    ssize_t written = ::write(fd, payload.data(), payload.size());
    int saved_errno = errno;
    ::close(fd);

    if (written < 0) {
        return -saved_errno;
    }
    if (static_cast<size_t>(written) != payload.size()) {
        return -EIO;
    }
    return 0;
}

std::string ReadAttr(const std::string& attr) {
    return ReadFile(SysfsPath(attr));
}

int ReadAttrInt(const std::string& attr) {
    return std::stoi(ReadAttr(attr));
}

SimtempStats ReadStats() {
    const std::string text = ReadAttr("stats");
    SimtempStats stats;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const long long value = std::stoll(line.substr(pos + 1));
        if (key == "total_samples") stats.total_samples = value;
        else if (key == "threshold_crossings") stats.threshold_crossings = value;
    }
    return stats;
}

bool WaitForSample(int fd, SimtempSample* out, int timeout_ms) {
    // Block until poll() says data is ready, respecting the provided timeout.
    struct pollfd pfd {
        fd, POLLIN | POLLRDNORM, 0
    };
    int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        return false;
    }
    if (!(pfd.revents & (POLLIN | POLLRDNORM))) {
        return false;
    }

    errno = 0;
    ssize_t n = ::read(fd, out, sizeof(*out));
    if (n != sizeof(*out)) {
        return false;
    }
    return true;
}

}  // namespace

class SimtempTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Bail out early if the module is not loaded or sysfs tree missing.
        if (!PathExists(kDevPath)) {
            GTEST_SKIP() << "/dev/simtemp not present; load module before running tests";
        }
        if (!PathExists(kSysfsBase)) {
            GTEST_SKIP() << "sysfs path missing; load module before running tests";
        }

        // Snapshot current settings so we can restore them in TearDown().
        original_sampling_ = ReadAttrInt("sampling_ms");
        original_threshold_ = ReadAttrInt("threshold_mC");
        original_mode_ = ReadAttr("mode");
        original_stats_ = ReadStats();

        // Keep the device file open for the duration of each test.
        dev_fd_ = ::open(kDevPath, O_RDONLY | O_CLOEXEC);
        if (dev_fd_ < 0) {
            GTEST_SKIP() << "Cannot open /dev/simtemp for reading (" << strerror(errno) << ")";
        }

        // Ensure we can write sysfs attributes (often requires sudo).
        int check = ::open(SysfsPath("sampling_ms").c_str(), O_WRONLY | O_CLOEXEC);
        if (check < 0) {
            GTEST_SKIP() << "Need write access to sysfs attributes (sudo?)";
        }
        ::close(check);
    }

    void TearDown() override {
        // Restore original configuration to avoid leaking state across runs.
        if (dev_fd_ >= 0) {
            WriteAttr("sampling_ms", std::to_string(original_sampling_));
            WriteAttr("threshold_mC", std::to_string(original_threshold_));
            WriteAttr("mode", original_mode_);
            ::close(dev_fd_);
            dev_fd_ = -1;
        }
    }

    void FlushDevice() {
        int flags = fcntl(dev_fd_, F_GETFL);
        if (flags < 0)
            return;
        fcntl(dev_fd_, F_SETFL, flags | O_NONBLOCK);
        SimtempSample dummy{};
        // Drain any pending samples so subsequent reads observe post-config data.
        while (true) {
            ssize_t n = ::read(dev_fd_, &dummy, sizeof(dummy));
            if (n != sizeof(dummy))
                break;
        }
        fcntl(dev_fd_, F_SETFL, flags);
    }

    int dev_fd_{-1};
    int original_sampling_{};
    int original_threshold_{};
    std::string original_mode_;
    SimtempStats original_stats_{};
};

TEST_F(SimtempTest, SysfsAttributeRoundTrip) {
    // Valid writes round-trip successfully.
    ASSERT_EQ(0, WriteAttr("sampling_ms", "250"));
    EXPECT_EQ(250, ReadAttrInt("sampling_ms"));

    ASSERT_EQ(0, WriteAttr("threshold_mC", "36000"));
    EXPECT_EQ(36000, ReadAttrInt("threshold_mC"));

    ASSERT_EQ(0, WriteAttr("mode", "ramp"));
    EXPECT_EQ("ramp", ReadAttr("mode"));

    // Out-of-range values should be rejected and leave previous values intact.
    EXPECT_EQ(-EINVAL, WriteAttr("sampling_ms", "0"));
    EXPECT_EQ(250, ReadAttrInt("sampling_ms"));

    EXPECT_EQ(-EINVAL, WriteAttr("threshold_mC", "999999"));
    EXPECT_EQ(36000, ReadAttrInt("threshold_mC"));

    EXPECT_EQ(-EINVAL, WriteAttr("mode", "invalid"));
    EXPECT_EQ("ramp", ReadAttr("mode"));
}

TEST_F(SimtempTest, SamplesContainExpectedFlagsAndRange) {
    // Ramp mode should periodically generate threshold crossing events.
    ASSERT_EQ(0, WriteAttr("mode", "ramp"));
    ASSERT_EQ(0, WriteAttr("sampling_ms", "5"));
    const int threshold = 30000;
    ASSERT_EQ(0, WriteAttr("threshold_mC", std::to_string(threshold)));

    // Allow new configuration to take effect.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    FlushDevice();

    int fd = dev_fd_;
    constexpr int kSampleBudget = 600;
    bool crossing_seen = false;
    bool has_previous = false;
    SimtempSample previous{};

    for (int i = 0; i < kSampleBudget; ++i) {
        SimtempSample s{};
        ASSERT_TRUE(WaitForSample(fd, &s, 200)) << "timeout waiting for sample " << i;
        EXPECT_NE(0u, s.flags & 0x1u) << "NEW_SAMPLE flag should be set";
        EXPECT_GE(s.temp_mC, 20000);
        EXPECT_LE(s.temp_mC, 45000);
        if (s.flags & 0x2u) {
            crossing_seen = true;
            if (has_previous) {
                // Ensure the flag corresponds to a sign change across the threshold.
                const long long prev_delta = static_cast<long long>(previous.temp_mC) - threshold;
                const long long curr_delta = static_cast<long long>(s.temp_mC) - threshold;
                EXPECT_LE(prev_delta * curr_delta, 0)
                    << "threshold flag should indicate sign change around threshold";
            }
            break;
        }
        previous = s;
        has_previous = true;
    }

    EXPECT_TRUE(crossing_seen) << "Expected at least one threshold crossing flag in ramp mode";
}

TEST_F(SimtempTest, PartialReadIsRejected) {
    // Requesting fewer bytes than a whole sample should return -EINVAL.
    int fd = ::open(kDevPath, O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0) << "Failed to open device";

    std::vector<char> buf(sizeof(SimtempSample) - 4, 0);
    errno = 0;
    ssize_t n = ::read(fd, buf.data(), buf.size());
    int err = errno;
    ::close(fd);

    EXPECT_EQ(-1, n);
    EXPECT_EQ(EINVAL, err);
}

TEST_F(SimtempTest, PollSignalsDataAvailable) {
    // poll() must report readable data once a sample arrives.
    ASSERT_EQ(0, WriteAttr("sampling_ms", "20"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    struct pollfd pfd {
        dev_fd_, POLLIN | POLLRDNORM, 0
    };
    int ret = ::poll(&pfd, 1, 500);
    ASSERT_GT(ret, 0) << "poll timed out";
    EXPECT_NE(0, pfd.revents & (POLLIN | POLLRDNORM));

    SimtempSample s{};
    ASSERT_TRUE(WaitForSample(dev_fd_, &s, 0));
    EXPECT_NE(0u, s.flags & 0x1u);
}

TEST_F(SimtempTest, StressReconfigureAndRead) {
    std::vector<int> sampling_values{10, 25, 50, 75, 100};
    std::vector<std::string> modes{"normal", "noisy", "ramp"};
    std::vector<int> thresholds{15000, 25000, 35000};

    SimtempStats before = ReadStats();
    int reads = 0;

    for (int i = 0; i < 15; ++i) {
        // Rapidly cycle through valid configurations.
        ASSERT_EQ(0, WriteAttr("sampling_ms", std::to_string(sampling_values[i % sampling_values.size()])));
        ASSERT_EQ(0, WriteAttr("mode", modes[i % modes.size()]));
        ASSERT_EQ(0, WriteAttr("threshold_mC", std::to_string(thresholds[i % thresholds.size()])));

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        SimtempSample s{};
        ASSERT_TRUE(WaitForSample(dev_fd_, &s, 500));
        ++reads;
        EXPECT_NE(0u, s.flags & 0x1u);
    }

    SimtempStats after = ReadStats();
    // Counters should move forward, even if not by an exact amount.
    EXPECT_GE(after.total_samples, before.total_samples);
    EXPECT_GE(after.threshold_crossings, before.threshold_crossings);
    EXPECT_GE(after.total_samples - before.total_samples, 1);
}
