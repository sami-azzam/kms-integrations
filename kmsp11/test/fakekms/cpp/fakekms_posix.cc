#include <csignal>

#include "absl/strings/str_format.h"
#include "glog/logging.h"
#include "gtest/gtest.h"
#include "kmsp11/test/fakekms/cpp/fakekms.h"
#include "kmsp11/test/runfiles.h"
#include "kmsp11/util/cleanup.h"
#include "kmsp11/util/status_macros.h"

namespace kmsp11 {

namespace {

class PosixFakeKms : public FakeKms {
 public:
  static absl::StatusOr<std::unique_ptr<PosixFakeKms>> New(
      absl::string_view flags);

  PosixFakeKms(std::string listen_addr, pid_t pid)
      : FakeKms(listen_addr), pid_(pid) {}

  ~PosixFakeKms() { CHECK_EQ(kill(pid_, SIGINT), 0); }

 private:
  pid_t pid_;
};

absl::Status PosixErrorToStatus(absl::string_view prefix) {
  return absl::InternalError(
      absl::StrFormat("%s: %s", prefix, strerror(errno)));
}

absl::StatusOr<std::unique_ptr<PosixFakeKms>> PosixFakeKms::New(
    absl::string_view flags) {
  int fd[2];
  if (pipe(fd) == -1) {
    return PosixErrorToStatus("unable to create output pipe");
  }
  Cleanup c([&fd]() {
    CHECK_EQ(close(fd[0]), 0);
    CHECK_EQ(close(fd[1]), 0);
  });

  pid_t pid = fork();
  switch (pid) {
    // fork failure
    case -1: {
      return PosixErrorToStatus("failure forking");
    }

    // post-fork child
    case 0: {
      if (dup2(fd[1], STDOUT_FILENO) == -1) {
        exit(1);
      }

      // we'll be replacing the executable, so cleanup must happen manually
      c.~Cleanup();

      std::string bin_path = RunfileLocation(
          "com_google_kmstools/kmsp11/test/fakekms/main/fakekms_/fakekms");
      std::string bin_flags(flags);
      execl(bin_path.c_str(), bin_path.c_str(), bin_flags.c_str(), (char*)0);

      // the previous line replaces the executable, so this
      // line shouldn't be reached
      exit(2);
    }

    // post-fork parent
    default: {
      FILE* file = fdopen(fd[0], "r");
      if (!file) {
        return PosixErrorToStatus("error opening pipe");
      }

      char* line = nullptr;
      size_t len = 0;
      if (getline(&line, &len, file) == -1) {
        free(line);
        return PosixErrorToStatus("failure reading address");
      }

      std::string address(line, len);
      free(line);
      return absl::make_unique<PosixFakeKms>(address, pid);
    }
  }
}

}  // namespace

absl::StatusOr<std::unique_ptr<FakeKms>> FakeKms::New(absl::string_view flags) {
  ASSIGN_OR_RETURN(std::unique_ptr<PosixFakeKms> fake,
                   PosixFakeKms::New(flags));
  return std::unique_ptr<FakeKms>(std::move(fake));
}

}  // namespace kmsp11