#include "src/stirling/pgsql/parse.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include <absl/strings/ascii.h>
#include <magic_enum.hpp>

#include "src/common/base/base.h"
#include "src/stirling/common/binary_decoder.h"

namespace pl {
namespace stirling {
namespace pgsql {

#define PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(expr, val_or) \
  PL_ASSIGN_OR(expr, val_or, return ParseState::kNeedsMoreData)

ParseState ParseRegularMessage(std::string_view* buf, RegularMessage* msg) {
  BinaryDecoder decoder(*buf);
  PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(const char char_val, decoder.ExtractChar());
  msg->tag = static_cast<Tag>(char_val);
  PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(msg->len, decoder.ExtractInt<int32_t>());

  constexpr int kLenFieldLen = 4;
  if (msg->len < kLenFieldLen) {
    // Len includes the len field itself, so its value cannot be less than the length of the field.
    return ParseState::kInvalid;
  }
  const size_t str_len = msg->len - 4;
  // Len includes the length field itself (int32_t), so the payload needs to exclude 4 bytes.
  PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(std::string_view str_view,
                                      decoder.ExtractString<char>(str_len));
  msg->payload = std::string(str_view);

  *buf = decoder.Buf();
  return ParseState::kSuccess;
}

ParseState ParseStartupMessage(std::string_view* buf, StartupMessage* msg) {
  BinaryDecoder decoder(*buf);

  PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(msg->len, decoder.ExtractInt<int32_t>());
  PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(msg->proto_ver.major, decoder.ExtractInt<int16_t>());
  PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(msg->proto_ver.minor, decoder.ExtractInt<int16_t>());

  const size_t kHeaderSize = 2 * sizeof(int32_t);

  if (decoder.BufSize() < msg->len - kHeaderSize) {
    return ParseState::kNeedsMoreData;
  }

  while (!decoder.eof()) {
    PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(std::string_view name, decoder.ExtractStringUtil('\0'));
    if (name.empty()) {
      // Each name or value is terminated by '\0'. And all name value pairs are terminated by an
      // additional '\0'.
      //
      // Extracting an empty name means we are at the end of the string.
      break;
    }
    PL_ASSIGN_OR_RETURN_NEEDS_MORE_DATA(std::string_view value, decoder.ExtractStringUtil('\0'));
    if (value.empty()) {
      return ParseState::kInvalid;
    }
    msg->nvs.push_back(NV{std::string(name), std::string(value)});
  }
  *buf = decoder.Buf();
  return ParseState::kSuccess;
}

size_t FindFrameBoundary(std::string_view buf, size_t start) {
  for (size_t i = start; i < buf.size(); ++i) {
    if (magic_enum::enum_cast<Tag>(buf[i]).has_value()) {
      return i;
    }
  }
  return std::string_view::npos;
}

namespace {

using MsgDeqIter = std::deque<RegularMessage>::iterator;

void AdvanceIterBeyondTimestamp(MsgDeqIter* start, const MsgDeqIter& end, uint64_t ts) {
  while (*start != end && (*start)->timestamp_ns < ts) {
    ++(*start);
  }
}

struct TagMatcher {
  explicit TagMatcher(std::set<Tag> tags) : target_tags(std::move(tags)) {}
  bool operator()(const RegularMessage& msg) {
    return target_tags.find(msg.tag) != target_tags.end();
  }
  std::set<Tag> target_tags;
};

#define PL_ASSIGN_OR_RETURN_RES(expr, val_or, res) PL_ASSIGN_OR(expr, val_or, return res)

}  // namespace

// Given the input as the payload of a kRowDesc message, returns a list of column name.
// Row description format:
// | int16 field count |
// | Field description |
// ...
// Field description format:
// | string name | int32 table ID | int16 column number | int32 type ID | int16 type size |
// | int32 type modifier | int16 format code (text|binary) |
std::vector<std::string_view> ParseRowDesc(std::string_view row_desc) {
  std::vector<std::string_view> res;

  BinaryDecoder decoder(row_desc);
  PL_ASSIGN_OR_RETURN_RES(const int16_t field_count, decoder.ExtractInt<int16_t>(), res);
  for (int i = 0; i < field_count; ++i) {
    PL_ASSIGN_OR_RETURN_RES(std::string_view col_name, decoder.ExtractStringUtil('\0'), res);

    if (col_name.empty()) {
      // Empty column name is invalid. Just put all remaining data as another name and return.
      VLOG(1) << "Encounter an empty column name on the column " << i;
      res.push_back(decoder.Buf());
      return res;
    }
    res.push_back(col_name);

    constexpr size_t kFieldDescSize = 3 * sizeof(int32_t) + 3 * sizeof(int16_t);
    if (decoder.BufSize() < kFieldDescSize) {
      VLOG(1) << absl::Substitute("Not enough data for parsing, needs $0 bytes, got $1",
                                  kFieldDescSize, decoder.BufSize());
      return res;
    }
    // Discard the reset of the message, which are not used.
    decoder.ExtractString<char>(kFieldDescSize);
  }
  return res;
}

std::vector<std::optional<std::string_view>> ParseDataRow(std::string_view data_row) {
  std::vector<std::optional<std::string_view>> res;

  BinaryDecoder decoder(data_row);
  PL_ASSIGN_OR_RETURN_RES(const int16_t field_count, decoder.ExtractInt<int16_t>(), res);
  for (int i = 0; i < field_count; ++i) {
    if (decoder.BufSize() < sizeof(int32_t)) {
      VLOG(1) << "Not enough data";
      return res;
    }
    // The length of the column value, in bytes (this count does not include itself). Can be zero.
    PL_ASSIGN_OR_RETURN_RES(int32_t value_len, decoder.ExtractInt<int32_t>(), res);
    // As a special case, -1 indicates a NULL column value. No value bytes follow in the NULL case.
    constexpr int kNullValLen = -1;
    if (value_len == kNullValLen) {
      res.push_back(std::nullopt);
      continue;
    }
    if (value_len == 0) {
      res.push_back({});
      continue;
    }
    if (decoder.BufSize() < static_cast<size_t>(value_len)) {
      VLOG(1) << "Not enough data, copy the rest of data";
      value_len = decoder.BufSize();
    }
    PL_ASSIGN_OR_RETURN_RES(std::string_view value, decoder.ExtractString<char>(value_len), res);
    res.push_back(value);
  }
  return res;
}

namespace {

template <typename TElemType>
class DequeView {
 public:
  using Iteraotr = typename std::deque<TElemType>::iterator;

  DequeView(Iteraotr begin, Iteraotr end)
      : size_(std::distance(begin, end)), begin_(begin), end_(end) {}

  const TElemType& operator[](size_t i) const { return *(begin_ + i); }
  const TElemType& Back() const { return *(end_ - 1); }
  const TElemType& Front() const { return *begin_; }
  size_t Size() const { return size_; }
  bool Empty() const { return size_ == 0; }

  const Iteraotr& Begin() const { return begin_; }
  const Iteraotr& End() const { return end_; }

 private:
  size_t size_ = 0;
  Iteraotr begin_;
  Iteraotr end_;
};

// Returns a list of messages that before ends with a kCmdComplete or kErrResp message.
DequeView<RegularMessage> GetCmdRespMsgs(MsgDeqIter* begin, MsgDeqIter end) {
  auto resp_iter = std::find_if(*begin, end, TagMatcher({Tag::kCmdComplete, Tag::kErrResp}));
  if (resp_iter == end) {
    return {end, end};
  }
  ++resp_iter;
  DequeView<RegularMessage> res(*begin, resp_iter);
  *begin = resp_iter;
  return res;
}

// Error message's payload has multiple following fields:
// | byte type | string value |
//
// TODO(yzhao): Do not call parsing functions. Check out frame_body_decoder.cc
// StatusOr<QueryReq> ParseQueryReq(Frame* frame) for parsing;
// and cql_stitcher.cc Status ProcessQueryReq(Frame* req_frame, Request* req) for stitching and
// formatting.
std::string_view FmtErrorResp(const RegularMessage& msg) {
  BinaryDecoder decoder(msg.payload);
  // Each field has at least 2 bytes, one for byte, another for string, which can be empty, but
  // always ends with '\0'.
  while (decoder.BufSize() >= 2) {
    PL_ASSIGN_OR_RETURN_RES(const char type, decoder.ExtractChar(), {});
    // See https://www.postgresql.org/docs/9.3/protocol-error-fields.html for the complete list of
    // error code.
    constexpr char kHumanReadableMessage = 'M';
    if (type == kHumanReadableMessage) {
      PL_ASSIGN_OR_RETURN_RES(std::string_view str_view, decoder.ExtractStringUtil('\0'), {});
      return str_view;
    }
  }
  return msg.payload;
}

// TODO(yzhao): Do not call parsing functions.
std::string FmtSelectResp(const DequeView<RegularMessage>& msgs) {
  auto row_desc_iter = std::find_if(msgs.Begin(), msgs.End(), TagMatcher({Tag::kRowDesc}));
  ECHECK(row_desc_iter != msgs.End()) << "Failed to find kRowDesc message in SELECT response.";

  std::string res;
  if (row_desc_iter != msgs.End()) {
    std::vector<std::string_view> row_descs = ParseRowDesc(row_desc_iter->payload);
    absl::StrAppend(&res, absl::StrJoin(row_descs, ","));
    absl::StrAppend(&res, "\n");
  }
  for (size_t i = 1; i < msgs.Size() - 1; ++i) {
    // kDataRow messages can mixed with other responses in an extended query, in which protocol
    // parsing, parameter binding, execution are separated into multiple request & response
    // exchanges.
    if (msgs[i].tag != Tag::kDataRow) {
      continue;
    }
    std::vector<std::optional<std::string_view>> data_row = ParseDataRow(msgs[i].payload);
    absl::StrAppend(&res,
                    absl::StrJoin(data_row, ",",
                                  [](std::string* out, const std::optional<std::string_view>& d) {
                                    out->append(d.has_value() ? d.value() : "[NULL]");
                                  }));
    absl::StrAppend(&res, "\n");
  }
  return res;
}

namespace cmd {

constexpr std::string_view kSelect = "SELECT";

}  // namespace cmd

// Query response messages end with a kCmdComplete message. Its payload determines the content prior
// to that message.
//
// See CommandComplete section in https://www.postgresql.org/docs/9.3/protocol-message-formats.html.
//
// TODO(yzhao): Format in JSON.
// TODO(yzhao): Do not call parsing code inside.
std::string FmtCmdResp(const DequeView<RegularMessage>& msgs) {
  const RegularMessage& cmd_complete_msg = msgs.Back();
  if (cmd_complete_msg.tag == Tag::kErrResp) {
    return std::string(FmtErrorResp(cmd_complete_msg));
  }
  // Non-SELECT response only has one kCmdComplete message. Output the payload directly.
  if (msgs.Size() == 1) {
    return msgs.Begin()->payload;
  }
  std::string res;
  if (absl::StartsWith(cmd_complete_msg.payload, cmd::kSelect)) {
    res = FmtSelectResp(msgs);
  }
  absl::StrAppend(&res, msgs.Back().payload);
  // TODO(yzhao): Need to test and handle other cases, if any.
  return res;
}

std::string_view StripZeroChar(std::string_view str) {
  while (!str.empty() && str.back() == '\0') {
    str.remove_suffix(1);
  }
  while (!str.empty() && str.front() == '\0') {
    str.remove_prefix(1);
  }
  return str;
}

void StripZeroChar(Record* record) {
  record->req.payload = StripZeroChar(record->req.payload);
  record->resp.payload = StripZeroChar(record->resp.payload);
}

}  // namespace

// Find the messages of query response. The result argument begin is advanced past the right most
// message being consumed.
//
// TODO(yzhao): Change this to use ContainerView<> as input.
StatusOr<RegularMessage> AssembleQueryResp(MsgDeqIter* begin, const MsgDeqIter& end) {
  DequeView<RegularMessage> msgs = GetCmdRespMsgs(begin, end);
  if (msgs.Empty()) {
    return error::InvalidArgument("Did not find kCmdComplete or kErrResp message");
  }
  std::string text = FmtCmdResp(msgs);
  RegularMessage msg = {};
  msg.timestamp_ns = msgs.Front().timestamp_ns;
  msg.payload = std::move(text);
  return msg;
}

// Returns all messages until the first kExecute message. Some of the returned messages are used to
// format a request message. As of 2020-04, only the first kParse message's payload is included in
// the request message, all others are discarded.
StatusOr<std::vector<RegularMessage>> GetParseReqMsgs(MsgDeqIter* begin, const MsgDeqIter& end) {
  auto kExec_iter = std::find_if(*begin, end, TagMatcher({Tag::kExecute}));
  if (kExec_iter == end) {
    return error::InvalidArgument("Could not find kExec message");
  }
  std::vector<RegularMessage> msgs;
  for (auto iter = *begin; iter != kExec_iter; ++iter) {
    msgs.push_back(std::move(*iter));
  }
  ++kExec_iter;
  *begin = kExec_iter;
  if (msgs.empty()) {
    return error::InvalidArgument("Did not find any messages");
  }
  return msgs;
}

RecordsWithErrorCount<pgsql::Record> ProcessFrames(std::deque<pgsql::RegularMessage>* reqs,
                                                   std::deque<pgsql::RegularMessage>* resps) {
  std::vector<pgsql::Record> records;
  int error_count = 0;
  auto req_iter = reqs->begin();
  auto resp_iter = resps->begin();
  // PostgreSQL query mode:
  //   In-order mode: where one query (one regular message) is followed one response (possibly with
  //   multiple regular messages).
  //   The code now can handle this mode.
  //
  //   Batch mode: Multiple queries are batched into a list, and sent to server; the responses are
  //   sent back in the same order as their corresponding queries.
  //   The code now can handle this mode.
  //
  //   Pipeline mode: Seem supported in PostgreSQL.
  //   https://2ndquadrant.github.io/postgres/libpq-batch-mode.html
  //   mentions pipelining. But the details are not clear yet.
  //
  // TODO(yzhao): Research batch and pipeline mode and confirm their behaviors.
  while (req_iter != reqs->end() && resp_iter != resps->end()) {
    // First advance response iterator to be at or later than the request's time stamp.
    // This can:
    // * Skip kReadyForQuery message, which appears before any of request messages.
    AdvanceIterBeyondTimestamp(&resp_iter, resps->end(), req_iter->timestamp_ns);

    // TODO(yzhao): Use a map to encode request type and the actions to find the response.
    // So we can get rid of the switch statement. That also include AdvanceIterBeyondTimestamp()
    // into the handler functions, such that the logic is more grouped.
    switch (req_iter->tag) {
      case Tag::kReadyForQuery:
        VLOG(1) << "Skip Tag::kReadyForQuery.";
        break;
      // NOTE: kPasswd message is a response by client to the authenticate request.
      // But it was still classified as request to server from client, according to our model.
      // And we just ignore such message, and kAuth message as well.
      case Tag::kPasswd:
        // Ignore auth response.
        ++req_iter;
        break;
      case Tag::kQuery: {
        auto query_resp_or = AssembleQueryResp(&resp_iter, resps->end());
        if (query_resp_or.ok()) {
          Record record{std::move(*req_iter), query_resp_or.ConsumeValueOrDie()};
          StripZeroChar(&record);
          records.push_back(std::move(record));
        } else {
          ++error_count;
          VLOG(1) << "Failed to assemble query response message, status: "
                  << query_resp_or.ToString();
        }
        ++req_iter;
        break;
      }
      case Tag::kParse: {
        auto req_msgs_or = GetParseReqMsgs(&req_iter, reqs->end());
        if (!req_msgs_or.ok()) {
          ++error_count;
          VLOG(1) << "Failed to assemble parse request messages, status: "
                  << req_msgs_or.ToString();
          break;
        }
        auto query_resp_or = AssembleQueryResp(&resp_iter, resps->end());
        if (!query_resp_or.ok()) {
          ++error_count;
          VLOG(1) << "Failed to assemble query response message, status: "
                  << query_resp_or.ToString();
          break;
        }
        Record record{std::move(req_msgs_or.ConsumeValueOrDie().front()),
                      query_resp_or.ConsumeValueOrDie()};
        StripZeroChar(&record);
        records.push_back(std::move(record));
        break;
      }
      default:
        LOG_FIRST_N(WARNING, 10) << "Unhandled or invalid tag: "
                                 << static_cast<char>(req_iter->tag);
        // By default for any message with unhandled or invalid tag, ignore and continue.
        // TODO(yzhao): Revise based on feedbacks.
        ++req_iter;
        break;
    }
  }
  reqs->erase(reqs->begin(), req_iter);
  resps->erase(resps->begin(), resp_iter);
  return {records, error_count};
}

}  // namespace pgsql

template <>
ParseState ParseFrame(MessageType type, std::string_view* buf, pgsql::RegularMessage* frame) {
  PL_UNUSED(type);

  std::string_view buf_copy = *buf;
  pgsql::StartupMessage startup_msg = {};
  if (ParseStartupMessage(&buf_copy, &startup_msg) == ParseState::kSuccess &&
      !startup_msg.nvs.empty()) {
    // Ignore startup message, but remove it from the buffer.
    *buf = buf_copy;
  }
  return ParseRegularMessage(buf, frame);
}

template <>
size_t FindFrameBoundary<pgsql::RegularMessage>(MessageType type, std::string_view buf,
                                                size_t start) {
  PL_UNUSED(type);
  return pgsql::FindFrameBoundary(buf, start);
}

RecordsWithErrorCount<pgsql::Record> ProcessFrames(std::deque<pgsql::RegularMessage>* reqs,
                                                   std::deque<pgsql::RegularMessage>* resps,
                                                   NoState* /*state*/) {
  return pgsql::ProcessFrames(reqs, resps);
}

}  // namespace stirling
}  // namespace pl
