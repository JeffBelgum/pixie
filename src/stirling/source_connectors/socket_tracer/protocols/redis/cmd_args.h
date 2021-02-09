#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/common/base/base.h"

namespace pl {
namespace stirling {
namespace protocols {
namespace redis {

// Describes the format of a single Redis command argument.
enum class Format {
  // This argument value must be specified, therefore the simplest to format.
  kFixed,

  // This argument value has 0 or more elements.
  kList,

  // This argument value can be omitted.
  kOpt,
};

// Describes a single argument.
struct ArgDesc {
  std::string_view name;
  Format format;

  std::string ToString() const {
    return absl::Substitute("[$0::$1]", name, magic_enum::enum_name(format));
  }
};

// Describes the arguments of a Redis command.
class CmdArgs {
 public:
  // Allows convenient initialization of kCmdList below.
  //
  // TODO(yzhao): Let the :redis_cmds_format_generator and gen_redis_cmds.sh to statically produces
  // the initialization code for kCmdList that fills in the command argument format as well.
  CmdArgs(std::initializer_list<const char*> cmd_args);

  // Formats the input argument value based on this detected format of this command.
  StatusOr<std::string> FmtArgs(VectorView<std::string> args) const;

  // Cannot move this out of class definition. Doing that gcc build fails because this is not used.
  std::string ToString() const {
    std::string cmd_arg_descs_str = "<null>";
    if (cmd_arg_descs_.has_value()) {
      cmd_arg_descs_str = absl::StrJoin(cmd_arg_descs_.value(), ", ",
                                        [](std::string* buf, const ArgDesc& arg_desc) {
                                          absl::StrAppend(buf, arg_desc.ToString());
                                        });
    }
    return absl::Substitute("cmd_args: $0 formats: $1", absl::StrJoin(cmd_args_, ", "),
                            cmd_arg_descs_str);
  }

 private:
  std::string_view cmd_name_;
  std::vector<std::string_view> cmd_args_;
  std::optional<std::vector<ArgDesc>> cmd_arg_descs_;
};

}  // namespace redis
}  // namespace protocols
}  // namespace stirling
}  // namespace pl
