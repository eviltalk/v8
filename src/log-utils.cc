// Copyright 2009 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/log-utils.h"

#include "src/assert-scope.h"
#include "src/base/platform/platform.h"
#include "src/objects-inl.h"
#include "src/string-stream.h"
#include "src/utils.h"
#include "src/version.h"

namespace v8 {
namespace internal {


const char* const Log::kLogToTemporaryFile = "&";
const char* const Log::kLogToConsole = "-";

Log::Log(Logger* logger)
    : is_stopped_(false),
      output_handle_(nullptr),
      message_buffer_(nullptr),
      logger_(logger) {}

void Log::Initialize(const char* log_file_name) {
  message_buffer_ = NewArray<char>(kMessageBufferSize);

  // --log-all enables all the log flags.
  if (FLAG_log_all) {
    FLAG_log_api = true;
    FLAG_log_code = true;
    FLAG_log_gc = true;
    FLAG_log_suspect = true;
    FLAG_log_handles = true;
    FLAG_log_internal_timer_events = true;
  }

  // --prof implies --log-code.
  if (FLAG_prof) FLAG_log_code = true;

  // If we're logging anything, we need to open the log file.
  if (Log::InitLogAtStart()) {
    if (strcmp(log_file_name, kLogToConsole) == 0) {
      OpenStdout();
    } else if (strcmp(log_file_name, kLogToTemporaryFile) == 0) {
      OpenTemporaryFile();
    } else {
      OpenFile(log_file_name);
    }

    if (output_handle_ != nullptr) {
      Log::MessageBuilder msg(this);
      if (strlen(Version::GetEmbedder()) == 0) {
        msg.Append("v8-version,%d,%d,%d,%d,%d", Version::GetMajor(),
                   Version::GetMinor(), Version::GetBuild(),
                   Version::GetPatch(), Version::IsCandidate());
      } else {
        msg.Append("v8-version,%d,%d,%d,%d,%s,%d", Version::GetMajor(),
                   Version::GetMinor(), Version::GetBuild(),
                   Version::GetPatch(), Version::GetEmbedder(),
                   Version::IsCandidate());
      }
      msg.WriteToLogFile();
    }
  }
}


void Log::OpenStdout() {
  DCHECK(!IsEnabled());
  output_handle_ = stdout;
}


void Log::OpenTemporaryFile() {
  DCHECK(!IsEnabled());
  output_handle_ = base::OS::OpenTemporaryFile();
}


void Log::OpenFile(const char* name) {
  DCHECK(!IsEnabled());
  output_handle_ = base::OS::FOpen(name, base::OS::LogFileOpenMode);
}

FILE* Log::Close() {
  FILE* result = nullptr;
  if (output_handle_ != nullptr) {
    if (strcmp(FLAG_logfile, kLogToTemporaryFile) != 0) {
      fclose(output_handle_);
    } else {
      result = output_handle_;
    }
  }
  output_handle_ = nullptr;

  DeleteArray(message_buffer_);
  message_buffer_ = nullptr;

  is_stopped_ = false;
  return result;
}


Log::MessageBuilder::MessageBuilder(Log* log)
  : log_(log),
    lock_guard_(&log_->mutex_),
    pos_(0) {
  DCHECK_NOT_NULL(log_->message_buffer_);
}


void Log::MessageBuilder::Append(const char* format, ...) {
  Vector<char> buf(log_->message_buffer_ + pos_,
                   Log::kMessageBufferSize - pos_);
  va_list args;
  va_start(args, format);
  AppendVA(format, args);
  va_end(args);
  DCHECK_LE(pos_, Log::kMessageBufferSize);
}


void Log::MessageBuilder::AppendVA(const char* format, va_list args) {
  Vector<char> buf(log_->message_buffer_ + pos_,
                   Log::kMessageBufferSize - pos_);
  int result = v8::internal::VSNPrintF(buf, format, args);

  // Result is -1 if output was truncated.
  if (result >= 0) {
    pos_ += result;
  } else {
    pos_ = Log::kMessageBufferSize;
  }
  DCHECK_LE(pos_, Log::kMessageBufferSize);
}


void Log::MessageBuilder::Append(const char c) {
  if (pos_ < Log::kMessageBufferSize) {
    log_->message_buffer_[pos_++] = c;
  }
  DCHECK_LE(pos_, Log::kMessageBufferSize);
}


void Log::MessageBuilder::AppendDoubleQuotedString(const char* string) {
  Append('"');
  for (const char* p = string; *p != '\0'; p++) {
    if (*p == '"') {
      Append('\\');
    }
    Append(*p);
  }
  Append('"');
}


void Log::MessageBuilder::Append(String* str) {
  DisallowHeapAllocation no_gc;  // Ensure string stay valid.
  int length = str->length();
  for (int i = 0; i < length; i++) {
    Append(static_cast<char>(str->Get(i)));
  }
}

void Log::MessageBuilder::AppendAddress(Address addr) {
  Append("0x%" V8PRIxPTR, reinterpret_cast<intptr_t>(addr));
}

void Log::MessageBuilder::AppendSymbolName(Symbol* symbol) {
  DCHECK(symbol);
  Append("symbol(");
  if (!symbol->name()->IsUndefined(symbol->GetIsolate())) {
    Append("\"");
    AppendDetailed(String::cast(symbol->name()), false);
    Append("\" ");
  }
  Append("hash %x)", symbol->Hash());
}


void Log::MessageBuilder::AppendDetailed(String* str, bool show_impl_info) {
  if (str == nullptr) return;
  DisallowHeapAllocation no_gc;  // Ensure string stay valid.
  int len = str->length();
  if (len > 0x1000)
    len = 0x1000;
  if (show_impl_info) {
    Append(str->IsOneByteRepresentation() ? 'a' : '2');
    if (StringShape(str).IsExternal())
      Append('e');
    if (StringShape(str).IsInternalized())
      Append('#');
    Append(":%i:", str->length());
  }
  for (int i = 0; i < len; i++) {
    uc32 c = str->Get(i);
    if (c > 0xff) {
      Append("\\u%04x", c);
    } else if (c < 32 || c > 126) {
      Append("\\x%02x", c);
    } else if (c == ',') {
      Append("\\,");
    } else if (c == '\\') {
      Append("\\\\");
    } else if (c == '\"') {
      Append("\"\"");
    } else {
      Append("%lc", c);
    }
  }
}

void Log::MessageBuilder::AppendUnbufferedHeapString(String* str) {
  if (str == nullptr) return;
  DisallowHeapAllocation no_gc;  // Ensure string stay valid.
  ScopedVector<char> buffer(16);
  int len = str->length();
  for (int i = 0; i < len; i++) {
    uc32 c = str->Get(i);
    if (c >= 32 && c <= 126) {
      if (c == '\"') {
        AppendUnbufferedCString("\"\"");
      } else if (c == '\\') {
        AppendUnbufferedCString("\\\\");
      } else {
        AppendUnbufferedChar(c);
      }
    } else if (c > 0xff) {
      int length = v8::internal::SNPrintF(buffer, "\\u%04x", c);
      DCHECK_EQ(6, length);
      log_->WriteToFile(buffer.start(), length);
    } else {
      DCHECK_LE(c, 0xffff);
      int length = v8::internal::SNPrintF(buffer, "\\x%02x", c);
      DCHECK_EQ(4, length);
      log_->WriteToFile(buffer.start(), length);
    }
  }
}

void Log::MessageBuilder::AppendUnbufferedChar(char c) {
  log_->WriteToFile(&c, 1);
}

void Log::MessageBuilder::AppendUnbufferedCString(const char* str) {
  log_->WriteToFile(str, static_cast<int>(strlen(str)));
}

void Log::MessageBuilder::AppendStringPart(const char* str, int len) {
  if (pos_ + len > Log::kMessageBufferSize) {
    len = Log::kMessageBufferSize - pos_;
    DCHECK_GE(len, 0);
    if (len == 0) return;
  }
  Vector<char> buf(log_->message_buffer_ + pos_,
                   Log::kMessageBufferSize - pos_);
  StrNCpy(buf, str, len);
  pos_ += len;
  DCHECK_LE(pos_, Log::kMessageBufferSize);
}

void Log::MessageBuilder::WriteToLogFile() {
  DCHECK_LE(pos_, Log::kMessageBufferSize);
  // Assert that we do not already have a new line at the end.
  DCHECK(pos_ == 0 || log_->message_buffer_[pos_ - 1] != '\n');
  if (pos_ == Log::kMessageBufferSize) pos_--;
  log_->message_buffer_[pos_++] = '\n';
  const int written = log_->WriteToFile(log_->message_buffer_, pos_);
  if (written != pos_) {
    log_->stop();
    log_->logger_->LogFailure();
  }
}

}  // namespace internal
}  // namespace v8
