// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "upb/util/def_to_proto.h"

#include <inttypes.h>
#include <math.h>

#include "upb/port/vsnprintf_compat.h"
#include "upb/reflection/enum_reserved_range.h"
#include "upb/reflection/extension_range.h"
#include "upb/reflection/internal/field_def.h"
#include "upb/reflection/internal/file_def.h"
#include "upb/reflection/message.h"
#include "upb/reflection/message_reserved_range.h"

// Must be last.
#include "upb/port/def.inc"

typedef struct {
  upb_Arena* arena;
  jmp_buf err;
} upb_ToProto_Context;

#define CHK_OOM(val) \
  if (!(val)) UPB_LONGJMP(ctx->err, 1);

// We want to copy the options verbatim into the destination options proto.
// We use serialize+parse as our deep copy.
#define SET_OPTIONS(proto, desc_type, options_type, src)                  \
  {                                                                       \
    size_t size;                                                          \
    /* MEM: could use a temporary arena here instead. */                  \
    char* pb = UPB_DESC_FN(options_type, _serialize)(src, ctx->arena, &size);\
    CHK_OOM(pb);                                                          \
    UPB_DESC(options_type)* dst =                                         \
        UPB_DESC_FN(options_type, _parse)(pb, size, ctx->arena);          \
    CHK_OOM(dst);                                                         \
    UPB_DESC_FN(desc_type, _set_options)(proto, dst);                     \
  }

static upb_StringView strviewdup2(upb_ToProto_Context* ctx,
                                  upb_StringView str) {
  char* p = upb_Arena_Malloc(ctx->arena, str.size);
  CHK_OOM(p);
  memcpy(p, str.data, str.size);
  return (upb_StringView){.data = p, .size = str.size};
}

static upb_StringView strviewdup(upb_ToProto_Context* ctx, const char* s) {
  return strviewdup2(ctx, (upb_StringView){.data = s, .size = strlen(s)});
}

static upb_StringView qual_dup(upb_ToProto_Context* ctx, const char* s) {
  size_t n = strlen(s);
  char* p = upb_Arena_Malloc(ctx->arena, n + 1);
  CHK_OOM(p);
  p[0] = '.';
  memcpy(p + 1, s, n);
  return (upb_StringView){.data = p, .size = n + 1};
}

UPB_PRINTF(2, 3)
static upb_StringView printf_dup(upb_ToProto_Context* ctx, const char* fmt,
                                 ...) {
  const size_t max = 32;
  char* p = upb_Arena_Malloc(ctx->arena, max);
  CHK_OOM(p);
  va_list args;
  va_start(args, fmt);
  size_t n = _upb_vsnprintf(p, max, fmt, args);
  va_end(args);
  UPB_ASSERT(n < max);
  return (upb_StringView){.data = p, .size = n};
}

static bool upb_isprint(char ch) { return ch >= 0x20 && ch <= 0x7f; }

static int special_escape(char ch) {
  switch (ch) {
    // This is the same set of special escapes recognized by
    // absl::CEscape().
    case '\n':
      return 'n';
    case '\r':
      return 'r';
    case '\t':
      return 't';
    case '\\':
      return '\\';
    case '\'':
      return '\'';
    case '"':
      return '"';
    default:
      return -1;
  }
}

static upb_StringView default_bytes(upb_ToProto_Context* ctx,
                                    upb_StringView val) {
  size_t n = 0;
  for (size_t i = 0; i < val.size; i++) {
    char ch = val.data[i];
    if (special_escape(ch) >= 0)
      n += 2;  // '\C'
    else if (upb_isprint(ch))
      n += 1;
    else
      n += 4;  // '\123'
  }
  char* p = upb_Arena_Malloc(ctx->arena, n);
  CHK_OOM(p);
  char* dst = p;
  const char* src = val.data;
  const char* end = src + val.size;
  while (src < end) {
    unsigned char ch = *src++;
    if (special_escape(ch) >= 0) {
      *dst++ = '\\';
      *dst++ = (char)special_escape(ch);
    } else if (upb_isprint(ch)) {
      *dst++ = ch;
    } else {
      *dst++ = '\\';
      *dst++ = '0' + (ch >> 6);
      *dst++ = '0' + ((ch >> 3) & 0x7);
      *dst++ = '0' + (ch & 0x7);
    }
  }
  return (upb_StringView){.data = p, .size = n};
}

static upb_StringView default_string(upb_ToProto_Context* ctx,
                                     const upb_FieldDef* f) {
  upb_MessageValue d = upb_FieldDef_Default(f);
  upb_CType type = upb_FieldDef_CType(f);

  if (type == kUpb_CType_Float || type == kUpb_CType_Double) {
    double val = type == kUpb_CType_Float ? d.float_val : d.double_val;
    if (val == INFINITY) {
      return strviewdup(ctx, "inf");
    } else if (val == -INFINITY) {
      return strviewdup(ctx, "-inf");
    } else if (val != val) {
      return strviewdup(ctx, "nan");
    }
  }

  switch (upb_FieldDef_CType(f)) {
    case kUpb_CType_Bool:
      return strviewdup(ctx, d.bool_val ? "true" : "false");
    case kUpb_CType_Enum: {
      const upb_EnumDef* e = upb_FieldDef_EnumSubDef(f);
      const upb_EnumValueDef* ev =
          upb_EnumDef_FindValueByNumber(e, d.int32_val);
      return strviewdup(ctx, upb_EnumValueDef_Name(ev));
    }
    case kUpb_CType_Int64:
      return printf_dup(ctx, "%" PRId64, d.int64_val);
    case kUpb_CType_UInt64:
      return printf_dup(ctx, "%" PRIu64, d.uint64_val);
    case kUpb_CType_Int32:
      return printf_dup(ctx, "%" PRId32, d.int32_val);
    case kUpb_CType_UInt32:
      return printf_dup(ctx, "%" PRIu32, d.uint32_val);
    case kUpb_CType_Float:
      return printf_dup(ctx, "%.9g", d.float_val);
    case kUpb_CType_Double:
      return printf_dup(ctx, "%.17g", d.double_val);
    case kUpb_CType_String:
      return strviewdup2(ctx, d.str_val);
    case kUpb_CType_Bytes:
      return default_bytes(ctx, d.str_val);
    default:
      UPB_UNREACHABLE();
  }
}

static UPB_DESC(DescriptorProto_ReservedRange)* resrange_toproto(
    upb_ToProto_Context* ctx, const upb_MessageReservedRange* r) {
  UPB_DESC(DescriptorProto_ReservedRange)* proto =
      UPB_DESC(DescriptorProto_ReservedRange_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(DescriptorProto_ReservedRange_set_start)(
      proto, upb_MessageReservedRange_Start(r));
  UPB_DESC(DescriptorProto_ReservedRange_set_end)(proto,
                                               upb_MessageReservedRange_End(r));

  return proto;
}

static UPB_DESC(EnumDescriptorProto_EnumReservedRange)* enumresrange_toproto(
    upb_ToProto_Context* ctx, const upb_EnumReservedRange* r) {
  UPB_DESC(EnumDescriptorProto_EnumReservedRange)* proto =
      UPB_DESC(EnumDescriptorProto_EnumReservedRange_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(EnumDescriptorProto_EnumReservedRange_set_start)(
      proto, upb_EnumReservedRange_Start(r));
  UPB_DESC(EnumDescriptorProto_EnumReservedRange_set_end)(
      proto, upb_EnumReservedRange_End(r));

  return proto;
}

static UPB_DESC(FieldDescriptorProto)* fielddef_toproto(upb_ToProto_Context* ctx,
                                                     const upb_FieldDef* f) {
  UPB_DESC(FieldDescriptorProto)* proto =
      UPB_DESC(FieldDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(FieldDescriptorProto_set_name)(proto,
                                       strviewdup(ctx, upb_FieldDef_Name(f)));
  UPB_DESC(FieldDescriptorProto_set_number)(proto, upb_FieldDef_Number(f));
  UPB_DESC(FieldDescriptorProto_set_label)(proto, upb_FieldDef_Label(f));
  UPB_DESC(FieldDescriptorProto_set_type)(proto, upb_FieldDef_Type(f));

  if (upb_FieldDef_HasJsonName(f)) {
    UPB_DESC(FieldDescriptorProto_set_json_name)(
        proto, strviewdup(ctx, upb_FieldDef_JsonName(f)));
  }

  if (upb_FieldDef_IsSubMessage(f)) {
    UPB_DESC(FieldDescriptorProto_set_type_name)(
        proto,
        qual_dup(ctx, upb_MessageDef_FullName(upb_FieldDef_MessageSubDef(f))));
  } else if (upb_FieldDef_CType(f) == kUpb_CType_Enum) {
    UPB_DESC(FieldDescriptorProto_set_type_name)(
        proto, qual_dup(ctx, upb_EnumDef_FullName(upb_FieldDef_EnumSubDef(f))));
  }

  if (upb_FieldDef_IsExtension(f)) {
    UPB_DESC(FieldDescriptorProto_set_extendee)(
        proto,
        qual_dup(ctx, upb_MessageDef_FullName(upb_FieldDef_ContainingType(f))));
  }

  if (upb_FieldDef_HasDefault(f)) {
    UPB_DESC(FieldDescriptorProto_set_default_value)(proto,
                                                  default_string(ctx, f));
  }

  const upb_OneofDef* o = upb_FieldDef_ContainingOneof(f);
  if (o) {
    UPB_DESC(FieldDescriptorProto_set_oneof_index)(proto, upb_OneofDef_Index(o));
  }

  if (_upb_FieldDef_IsProto3Optional(f)) {
    UPB_DESC(FieldDescriptorProto_set_proto3_optional)(proto, true);
  }

  if (upb_FieldDef_HasOptions(f)) {
    SET_OPTIONS(proto, FieldDescriptorProto, FieldOptions,
                upb_FieldDef_Options(f));
  }

  return proto;
}

static UPB_DESC(OneofDescriptorProto)* oneofdef_toproto(upb_ToProto_Context* ctx,
                                                     const upb_OneofDef* o) {
  UPB_DESC(OneofDescriptorProto)* proto =
      UPB_DESC(OneofDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(OneofDescriptorProto_set_name)(proto,
                                       strviewdup(ctx, upb_OneofDef_Name(o)));

  if (upb_OneofDef_HasOptions(o)) {
    SET_OPTIONS(proto, OneofDescriptorProto, OneofOptions,
                upb_OneofDef_Options(o));
  }

  return proto;
}

static UPB_DESC(EnumValueDescriptorProto)* enumvaldef_toproto(
    upb_ToProto_Context* ctx, const upb_EnumValueDef* e) {
  UPB_DESC(EnumValueDescriptorProto)* proto =
      UPB_DESC(EnumValueDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(EnumValueDescriptorProto_set_name)(
      proto, strviewdup(ctx, upb_EnumValueDef_Name(e)));
  UPB_DESC(EnumValueDescriptorProto_set_number)(proto, upb_EnumValueDef_Number(e));

  if (upb_EnumValueDef_HasOptions(e)) {
    SET_OPTIONS(proto, EnumValueDescriptorProto, EnumValueOptions,
                upb_EnumValueDef_Options(e));
  }

  return proto;
}

static UPB_DESC(EnumDescriptorProto)* enumdef_toproto(upb_ToProto_Context* ctx,
                                                   const upb_EnumDef* e) {
  UPB_DESC(EnumDescriptorProto)* proto =
      UPB_DESC(EnumDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(EnumDescriptorProto_set_name)(proto,
                                      strviewdup(ctx, upb_EnumDef_Name(e)));

  int n = upb_EnumDef_ValueCount(e);
  UPB_DESC(EnumValueDescriptorProto)** vals =
      UPB_DESC(EnumDescriptorProto_resize_value)(proto, n, ctx->arena);
  CHK_OOM(vals);
  for (int i = 0; i < n; i++) {
    vals[i] = enumvaldef_toproto(ctx, upb_EnumDef_Value(e, i));
  }

  n = upb_EnumDef_ReservedRangeCount(e);
  UPB_DESC(EnumDescriptorProto_EnumReservedRange)** res_ranges =
      UPB_DESC(EnumDescriptorProto_resize_reserved_range)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    res_ranges[i] = enumresrange_toproto(ctx, upb_EnumDef_ReservedRange(e, i));
  }

  n = upb_EnumDef_ReservedNameCount(e);
  upb_StringView* res_names =
      UPB_DESC(EnumDescriptorProto_resize_reserved_name)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    res_names[i] = upb_EnumDef_ReservedName(e, i);
  }

  if (upb_EnumDef_HasOptions(e)) {
    SET_OPTIONS(proto, EnumDescriptorProto, EnumOptions,
                upb_EnumDef_Options(e));
  }

  return proto;
}

static UPB_DESC(DescriptorProto_ExtensionRange)* extrange_toproto(
    upb_ToProto_Context* ctx, const upb_ExtensionRange* e) {
  UPB_DESC(DescriptorProto_ExtensionRange)* proto =
      UPB_DESC(DescriptorProto_ExtensionRange_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(DescriptorProto_ExtensionRange_set_start)(proto,
                                                  upb_ExtensionRange_Start(e));
  UPB_DESC(DescriptorProto_ExtensionRange_set_end)(proto,
                                                upb_ExtensionRange_End(e));

  if (upb_ExtensionRange_HasOptions(e)) {
    SET_OPTIONS(proto, DescriptorProto_ExtensionRange, ExtensionRangeOptions,
                upb_ExtensionRange_Options(e));
  }

  return proto;
}

static UPB_DESC(DescriptorProto)* msgdef_toproto(upb_ToProto_Context* ctx,
                                              const upb_MessageDef* m) {
  UPB_DESC(DescriptorProto)* proto = UPB_DESC(DescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(DescriptorProto_set_name)(proto,
                                  strviewdup(ctx, upb_MessageDef_Name(m)));

  int n;

  n = upb_MessageDef_FieldCount(m);
  UPB_DESC(FieldDescriptorProto)** fields =
      UPB_DESC(DescriptorProto_resize_field)(proto, n, ctx->arena);
  CHK_OOM(fields);
  for (int i = 0; i < n; i++) {
    fields[i] = fielddef_toproto(ctx, upb_MessageDef_Field(m, i));
  }

  n = upb_MessageDef_OneofCount(m);
  UPB_DESC(OneofDescriptorProto)** oneofs =
      UPB_DESC(DescriptorProto_resize_oneof_decl)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    oneofs[i] = oneofdef_toproto(ctx, upb_MessageDef_Oneof(m, i));
  }

  n = upb_MessageDef_NestedMessageCount(m);
  UPB_DESC(DescriptorProto)** nested_msgs =
      UPB_DESC(DescriptorProto_resize_nested_type)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    nested_msgs[i] = msgdef_toproto(ctx, upb_MessageDef_NestedMessage(m, i));
  }

  n = upb_MessageDef_NestedEnumCount(m);
  UPB_DESC(EnumDescriptorProto)** nested_enums =
      UPB_DESC(DescriptorProto_resize_enum_type)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    nested_enums[i] = enumdef_toproto(ctx, upb_MessageDef_NestedEnum(m, i));
  }

  n = upb_MessageDef_NestedExtensionCount(m);
  UPB_DESC(FieldDescriptorProto)** nested_exts =
      UPB_DESC(DescriptorProto_resize_extension)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    nested_exts[i] =
        fielddef_toproto(ctx, upb_MessageDef_NestedExtension(m, i));
  }

  n = upb_MessageDef_ExtensionRangeCount(m);
  UPB_DESC(DescriptorProto_ExtensionRange)** ext_ranges =
      UPB_DESC(DescriptorProto_resize_extension_range)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    ext_ranges[i] = extrange_toproto(ctx, upb_MessageDef_ExtensionRange(m, i));
  }

  n = upb_MessageDef_ReservedRangeCount(m);
  UPB_DESC(DescriptorProto_ReservedRange)** res_ranges =
      UPB_DESC(DescriptorProto_resize_reserved_range)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    res_ranges[i] = resrange_toproto(ctx, upb_MessageDef_ReservedRange(m, i));
  }

  n = upb_MessageDef_ReservedNameCount(m);
  upb_StringView* res_names =
      UPB_DESC(DescriptorProto_resize_reserved_name)(proto, n, ctx->arena);
  for (int i = 0; i < n; i++) {
    res_names[i] = upb_MessageDef_ReservedName(m, i);
  }

  if (upb_MessageDef_HasOptions(m)) {
    SET_OPTIONS(proto, DescriptorProto, MessageOptions,
                upb_MessageDef_Options(m));
  }

  return proto;
}

static UPB_DESC(MethodDescriptorProto)* methoddef_toproto(upb_ToProto_Context* ctx,
                                                       const upb_MethodDef* m) {
  UPB_DESC(MethodDescriptorProto)* proto =
      UPB_DESC(MethodDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(MethodDescriptorProto_set_name)(proto,
                                        strviewdup(ctx, upb_MethodDef_Name(m)));

  UPB_DESC(MethodDescriptorProto_set_input_type)(
      proto,
      qual_dup(ctx, upb_MessageDef_FullName(upb_MethodDef_InputType(m))));
  UPB_DESC(MethodDescriptorProto_set_output_type)(
      proto,
      qual_dup(ctx, upb_MessageDef_FullName(upb_MethodDef_OutputType(m))));

  if (upb_MethodDef_ClientStreaming(m)) {
    UPB_DESC(MethodDescriptorProto_set_client_streaming)(proto, true);
  }

  if (upb_MethodDef_ServerStreaming(m)) {
    UPB_DESC(MethodDescriptorProto_set_server_streaming)(proto, true);
  }

  if (upb_MethodDef_HasOptions(m)) {
    SET_OPTIONS(proto, MethodDescriptorProto, MethodOptions,
                upb_MethodDef_Options(m));
  }

  return proto;
}

static UPB_DESC(ServiceDescriptorProto)* servicedef_toproto(
    upb_ToProto_Context* ctx, const upb_ServiceDef* s) {
  UPB_DESC(ServiceDescriptorProto)* proto =
      UPB_DESC(ServiceDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(ServiceDescriptorProto_set_name)(
      proto, strviewdup(ctx, upb_ServiceDef_Name(s)));

  size_t n = upb_ServiceDef_MethodCount(s);
  UPB_DESC(MethodDescriptorProto)** methods =
      UPB_DESC(ServiceDescriptorProto_resize_method)(proto, n, ctx->arena);
  for (size_t i = 0; i < n; i++) {
    methods[i] = methoddef_toproto(ctx, upb_ServiceDef_Method(s, i));
  }

  if (upb_ServiceDef_HasOptions(s)) {
    SET_OPTIONS(proto, ServiceDescriptorProto, ServiceOptions,
                upb_ServiceDef_Options(s));
  }

  return proto;
}

static UPB_DESC(FileDescriptorProto)* filedef_toproto(upb_ToProto_Context* ctx,
                                                   const upb_FileDef* f) {
  UPB_DESC(FileDescriptorProto)* proto =
      UPB_DESC(FileDescriptorProto_new)(ctx->arena);
  CHK_OOM(proto);

  UPB_DESC(FileDescriptorProto_set_name)(proto,
                                      strviewdup(ctx, upb_FileDef_Name(f)));

  const char* package = upb_FileDef_Package(f);
  if (package) {
    size_t n = strlen(package);
    if (n) {
      UPB_DESC(FileDescriptorProto_set_package)(proto, strviewdup(ctx, package));
    }
  }

  if (upb_FileDef_Syntax(f) == kUpb_Syntax_Editions) {
    UPB_DESC(FileDescriptorProto_set_edition)(proto, upb_FileDef_Edition(f));
  }

  if (upb_FileDef_Syntax(f) == kUpb_Syntax_Proto3) {
    UPB_DESC(FileDescriptorProto_set_syntax)(proto, strviewdup(ctx, "proto3"));
  }

  size_t n;
  n = upb_FileDef_DependencyCount(f);
  upb_StringView* deps =
      UPB_DESC(FileDescriptorProto_resize_dependency)(proto, n, ctx->arena);
  for (size_t i = 0; i < n; i++) {
    deps[i] = strviewdup(ctx, upb_FileDef_Name(upb_FileDef_Dependency(f, i)));
  }

  n = upb_FileDef_PublicDependencyCount(f);
  int32_t* public_deps =
      UPB_DESC(FileDescriptorProto_resize_public_dependency)(proto, n, ctx->arena);
  const int32_t* public_dep_nums = _upb_FileDef_PublicDependencyIndexes(f);
  if (n) memcpy(public_deps, public_dep_nums, n * sizeof(int32_t));

  n = upb_FileDef_WeakDependencyCount(f);
  int32_t* weak_deps =
      UPB_DESC(FileDescriptorProto_resize_weak_dependency)(proto, n, ctx->arena);
  const int32_t* weak_dep_nums = _upb_FileDef_WeakDependencyIndexes(f);
  if (n) memcpy(weak_deps, weak_dep_nums, n * sizeof(int32_t));

  n = upb_FileDef_TopLevelMessageCount(f);
  UPB_DESC(DescriptorProto)** msgs =
      UPB_DESC(FileDescriptorProto_resize_message_type)(proto, n, ctx->arena);
  for (size_t i = 0; i < n; i++) {
    msgs[i] = msgdef_toproto(ctx, upb_FileDef_TopLevelMessage(f, i));
  }

  n = upb_FileDef_TopLevelEnumCount(f);
  UPB_DESC(EnumDescriptorProto)** enums =
      UPB_DESC(FileDescriptorProto_resize_enum_type)(proto, n, ctx->arena);
  for (size_t i = 0; i < n; i++) {
    enums[i] = enumdef_toproto(ctx, upb_FileDef_TopLevelEnum(f, i));
  }

  n = upb_FileDef_ServiceCount(f);
  UPB_DESC(ServiceDescriptorProto)** services =
      UPB_DESC(FileDescriptorProto_resize_service)(proto, n, ctx->arena);
  for (size_t i = 0; i < n; i++) {
    services[i] = servicedef_toproto(ctx, upb_FileDef_Service(f, i));
  }

  n = upb_FileDef_TopLevelExtensionCount(f);
  UPB_DESC(FieldDescriptorProto)** exts =
      UPB_DESC(FileDescriptorProto_resize_extension)(proto, n, ctx->arena);
  for (size_t i = 0; i < n; i++) {
    exts[i] = fielddef_toproto(ctx, upb_FileDef_TopLevelExtension(f, i));
  }

  if (upb_FileDef_HasOptions(f)) {
    SET_OPTIONS(proto, FileDescriptorProto, FileOptions,
                upb_FileDef_Options(f));
  }

  return proto;
}

static UPB_DESC(DescriptorProto)* upb_ToProto_ConvertMessageDef(
    upb_ToProto_Context* const ctx, const upb_MessageDef* const m) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return msgdef_toproto(ctx, m);
}

UPB_DESC(DescriptorProto)* upb_MessageDef_ToProto(const upb_MessageDef* m,
                                               upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertMessageDef(&ctx, m);
}

UPB_DESC(EnumDescriptorProto)* upb_ToProto_ConvertEnumDef(
    upb_ToProto_Context* const ctx, const upb_EnumDef* const e) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return enumdef_toproto(ctx, e);
}

UPB_DESC(EnumDescriptorProto)* upb_EnumDef_ToProto(const upb_EnumDef* e,
                                                upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertEnumDef(&ctx, e);
}

UPB_DESC(EnumValueDescriptorProto)* upb_ToProto_ConvertEnumValueDef(
    upb_ToProto_Context* const ctx, const upb_EnumValueDef* e) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return enumvaldef_toproto(ctx, e);
}

UPB_DESC(EnumValueDescriptorProto)* upb_EnumValueDef_ToProto(
    const upb_EnumValueDef* e, upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertEnumValueDef(&ctx, e);
}

UPB_DESC(FieldDescriptorProto)* upb_ToProto_ConvertFieldDef(
    upb_ToProto_Context* const ctx, const upb_FieldDef* f) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return fielddef_toproto(ctx, f);
}

UPB_DESC(FieldDescriptorProto)* upb_FieldDef_ToProto(const upb_FieldDef* f,
                                                  upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertFieldDef(&ctx, f);
}

UPB_DESC(OneofDescriptorProto)* upb_ToProto_ConvertOneofDef(
    upb_ToProto_Context* const ctx, const upb_OneofDef* o) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return oneofdef_toproto(ctx, o);
}

UPB_DESC(OneofDescriptorProto)* upb_OneofDef_ToProto(const upb_OneofDef* o,
                                                  upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertOneofDef(&ctx, o);
}

UPB_DESC(FileDescriptorProto)* upb_ToProto_ConvertFileDef(
    upb_ToProto_Context* const ctx, const upb_FileDef* const f) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return filedef_toproto(ctx, f);
}

UPB_DESC(FileDescriptorProto)* upb_FileDef_ToProto(const upb_FileDef* f,
                                                upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertFileDef(&ctx, f);
}

UPB_DESC(MethodDescriptorProto)* upb_ToProto_ConvertMethodDef(
    upb_ToProto_Context* const ctx, const upb_MethodDef* m) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return methoddef_toproto(ctx, m);
}

UPB_DESC(MethodDescriptorProto)* upb_MethodDef_ToProto(
    const upb_MethodDef* const m, upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertMethodDef(&ctx, m);
}

UPB_DESC(ServiceDescriptorProto)* upb_ToProto_ConvertServiceDef(
    upb_ToProto_Context* const ctx, const upb_ServiceDef* const s) {
  if (UPB_SETJMP(ctx->err)) return NULL;
  return servicedef_toproto(ctx, s);
}

UPB_DESC(ServiceDescriptorProto)* upb_ServiceDef_ToProto(const upb_ServiceDef* s,
                                                      upb_Arena* a) {
  upb_ToProto_Context ctx = {a};
  return upb_ToProto_ConvertServiceDef(&ctx, s);
}
