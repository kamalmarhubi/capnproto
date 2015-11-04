// Copyright (c) 2015 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "json.h"
#include <cstdlib>  // std::strtod
#include <unordered_map>
#include <capnp/orphan.h>
#include <kj/debug.h>
#include <kj/function.h>
#include <kj/vector.h>

namespace capnp {

namespace {

struct TypeHash {
  size_t operator()(const Type& type) const {
    return type.hashCode();
  }
};

struct FieldHash {
  size_t operator()(const StructSchema::Field& field) const {
    return field.getIndex() ^ field.getContainingStruct().getProto().getId();
  }
};

}  // namespace

namespace _ {  // private

class Parser {
public:
  Parser(kj::ArrayPtr<const char> input) : input_(input), pos_(0) {}
  void parse(JsonValue::Builder &output) {
    KJ_REQUIRE(pos_ < input_.size(), "JSON message ends prematurely.");

    consumeWhitespace();

    switch (nextChar()) {
      case 'n': consume(NULL_); output.setNull(); break;
      case 'f': consume(FALSE); output.setBoolean(false); break;
      case 't': consume(TRUE); output.setBoolean(true); break;
      case '"': parseString(output); break;
      case '[': parseArray(output); break;
      case '{': parseObject(output); break;
      // TODO(soon): are there other possible leading characters?
      default: parseNumber(output); break;
    }
  }

  void advanceTo(size_t newPos) {
    KJ_REQUIRE(newPos < input_.size());
    pos_ = newPos;
  }

  void parseNumber(JsonValue::Builder &output) {
    // TODO(soon): strtod is more liberal than JSON grammar, eg allows leading +
    // strtod consumes leading whitespace, so we don't have to.
    char *numEnd;
    output.setNumber(std::strtod(input_.begin() + pos_, &numEnd));

    advanceTo(numEnd - input_.begin());
  }

  char nextChar() {
    return input_[pos_];
  }

  void consumeWithSurroundingWhitespace(char chr) {
    consumeWhitespace();
    consume(chr);
    consumeWhitespace();
  }

  void parseObject(JsonValue::Builder &output) {
    kj::Vector<Orphan<JsonValue::Field>> fields;
    auto orphanage = Orphanage::getForMessageContaining(output);

    consumeWithSurroundingWhitespace('{');
    while (consumeWhitespace(), nextChar() != '}') {
      auto orphan = orphanage.newOrphan<JsonValue::Field>();
      auto builder = orphan.get();

      builder.setName(consumeQuotedString());
      consumeWithSurroundingWhitespace(':');
      auto valueBuilder = builder.getValue();
      parse(valueBuilder);

      fields.add(kj::mv(orphan));

      if (consumeWhitespace(), nextChar() != '}') {
        consume(',');
      }
    }

    output.initObject(fields.size());
    auto object = output.getObject();

    for (size_t i = 0; i < fields.size(); ++i) {
      object.adoptWithCaveats(i, kj::mv(fields[i]));
    }

    consumeWithSurroundingWhitespace('}');
  }

  kj::String consumeQuotedString() {
    consume('"');
    // TODO(perf): avoid copy / alloc if no escapes encoutered.
    // TODO(perf): get statistics on string size and preallocate?
    kj::Vector<char> decoded;

    do {
      auto stringValue = consumeWhile([](const char chr) {
          return chr != '"' && chr != '\\';
      });

      decoded.addAll(stringValue);

      if (nextChar() == '\\') {  // handle escapes.
        advance(1);
        switch(nextChar()) {
          case '"' : decoded.add('"' ); advance(1); break;
          case '\\': decoded.add('\\'); advance(1); break;
          case '/' : decoded.add('/' ); advance(1); break;
          case 'b' : decoded.add('\b'); advance(1); break;
          case 'f' : decoded.add('\f'); advance(1); break;
          case 'n' : decoded.add('\n'); advance(1); break;
          case 'r' : decoded.add('\r'); advance(1); break;
          case 't' : decoded.add('\t'); advance(1); break;
          case 'u' : KJ_FAIL_REQUIRE("unicode escapes are not supported (yet!)");
          default: KJ_FAIL_REQUIRE("invalid escape", nextChar()); break;
        }
      }

    } while(nextChar() != '"');

    consume('"');
    decoded.add('\0');

    // TODO(perf): this copy can be eliminated, but I can't find the kj::wayToDoIt();
    return kj::String(decoded.releaseAsArray());
  }

  void parseString(JsonValue::Builder &output) {
    output.setString(consumeQuotedString());
  }

  bool maybeConsume(char chr) {
    if (nextChar() == chr) {
      consume(chr);
      return true;
    } else {
      return false;
    }
  }
  
  kj::ArrayPtr<const char> consumeNumber() {
    // TODO(soon): this is a mess; see what jq's parser does for numbers
    auto origPos = pos_;
    bool hasSign = nextChar() == '+' || nextChar() == '-';
    if (hasSign) { advance(1); }

    auto integralPart = consumeWhile([](char chr) {
      return (
        chr == '0' ||
        chr == '1' ||
        chr == '2' ||
        chr == '3' ||
        chr == '4' ||
        chr == '5' ||
        chr == '6' ||
        chr == '7' ||
        chr == '8' ||
        chr == '9'
      );
    });

    bool hasPoint = maybeConsume('.');
    kj::ArrayPtr<const char> fracPart;
    if (hasPoint) {
      fracPart = consumeWhile([](char chr) {
          return (
              chr == '0' ||
              chr == '1' ||
              chr == '2' ||
              chr == '3' ||
              chr == '4' ||
              chr == '5' ||
              chr == '6' ||
              chr == '7' ||
              chr == '8' ||
              chr == '9'
              );
          });
    }

    auto numPart = integralPart;

    if (hasSign) {
      numPart = kj::ArrayPtr<const char>(input_.begin() + origPos, numPart.end());
    }

    if (hasPoint) {
      numPart = kj::ArrayPtr<const char>(numPart.begin(), fracPart.end());
    }

    return numPart;
  }

  void consumeWhitespace() {
    consumeWhile([](char chr) {
      return (
        chr == ' '  ||
        chr == '\f' ||
        chr == '\n' ||
        chr == '\r' ||
        chr == '\t' ||
        chr == '\v'
      );
    });
  }

  void parseArray(JsonValue::Builder &output) {
    kj::Vector<Orphan<JsonValue>> values;
    auto orphanage = Orphanage::getForMessageContaining(output);

    // TODO(soon): this should be cleaned up
    consume('[');
    while (consumeWhitespace(), nextChar() != ']') {
      auto orphan = orphanage.newOrphan<JsonValue>();
      auto builder = orphan.get();
      parse(builder);
      values.add(kj::mv(orphan));


      if (consumeWhitespace(), nextChar() != ']') {
        consume(',');
      }
    }

    output.initArray(values.size());
    auto array = output.getArray();

    for (size_t i = 0; i < values.size(); ++i) {
      array.adoptWithCaveats(i, kj::mv(values[i]));
    }

    consume(']');
  }

  void advance(size_t numBytes) {
    KJ_REQUIRE(pos_ + numBytes < input_.size());
    pos_ += numBytes;
  }

  kj::ArrayPtr<const char> consumeWhile(kj::Function<bool(char)> predicate) {
    auto originalPos = pos_;
    while (predicate(input_[pos_])) { ++pos_; }

    return input_.slice(originalPos, pos_);
  }

  kj::ArrayPtr<const char> consume(char chr) {
    char current = input_[pos_];
    KJ_REQUIRE(current == chr, current, chr);

    auto ret = input_.slice(pos_, pos_ + 1);
    ++pos_;

    return ret;
  }

  kj::ArrayPtr<const char> consume(kj::ArrayPtr<const char> str) {
    // TODO(soon): check input has enough characters left
    auto originalPos = pos_;

    auto prefix = input_.slice(pos_, pos_ + str.size());

    // TODO(soon): error message
    KJ_REQUIRE(prefix == str, "Unexpected input in JSON message.", prefix, str);

    advance(str.size());
    return input_.slice(originalPos, originalPos + str.size());
  }


private:
  kj::ArrayPtr<const char> input_;
  size_t pos_;

  static const kj::ArrayPtr<const char> NULL_;
  static const kj::ArrayPtr<const char> FALSE;
  static const kj::ArrayPtr<const char> TRUE;

};


const kj::ArrayPtr<const char> Parser::NULL_ = kj::ArrayPtr<const char>({'n','u','l','l'});
const kj::ArrayPtr<const char> Parser::FALSE = kj::ArrayPtr<const char>({'f','a','l','s','e'});
const kj::ArrayPtr<const char> Parser::TRUE = kj::ArrayPtr<const char>({'t','r','u','e'});

}  // namespace _ (private)

struct JsonCodec::Impl {
  bool prettyPrint = false;

  std::unordered_map<Type, HandlerBase*, TypeHash> typeHandlers;
  std::unordered_map<StructSchema::Field, HandlerBase*, FieldHash> fieldHandlers;

  kj::StringTree encodeRaw(JsonValue::Reader value, uint indent, bool& multiline,
                           bool hasPrefix) const {
    switch (value.which()) {
      case JsonValue::NULL_:
        return kj::strTree("null");
      case JsonValue::BOOLEAN:
        return kj::strTree(value.getBoolean());
      case JsonValue::NUMBER:
        return kj::strTree(value.getNumber());

      case JsonValue::STRING:
        return kj::strTree(encodeString(value.getString()));

      case JsonValue::ARRAY: {
        auto array = value.getArray();
        uint subIndent = indent + (array.size() > 1);
        bool childMultiline = false;
        auto encodedElements = KJ_MAP(element, array) {
          return encodeRaw(element, subIndent, childMultiline, false);
        };

        return kj::strTree('[', encodeList(
            kj::mv(encodedElements), childMultiline, indent, multiline, hasPrefix), ']');
      }

      case JsonValue::OBJECT: {
        auto object = value.getObject();
        uint subIndent = indent + (object.size() > 1);
        bool childMultiline = false;
        kj::StringPtr colon = prettyPrint ? ": " : ":";
        auto encodedElements = KJ_MAP(field, object) {
          return kj::strTree(
              encodeString(field.getName()), colon,
              encodeRaw(field.getValue(), subIndent, childMultiline, true));
        };

        return kj::strTree('{', encodeList(
            kj::mv(encodedElements), childMultiline, indent, multiline, hasPrefix), '}');
      }

      case JsonValue::CALL: {
        auto call = value.getCall();
        auto params = call.getParams();
        uint subIndent = indent + (params.size() > 1);
        bool childMultiline = false;
        auto encodedElements = KJ_MAP(element, params) {
          return encodeRaw(element, subIndent, childMultiline, false);
        };

        return kj::strTree(call.getFunction(), '(', encodeList(
            kj::mv(encodedElements), childMultiline, indent, multiline, true), ')');
      }
    }

    KJ_FAIL_ASSERT("unknown JsonValue type", static_cast<uint>(value.which()));
  }

  kj::String encodeString(kj::StringPtr chars) const {
    static const char HEXDIGITS[] = "0123456789abcdef";
    kj::Vector<char> escaped(chars.size() + 3);

    escaped.add('"');
    for (char c: chars) {
      switch (c) {
        case '\"': escaped.addAll(kj::StringPtr("\\\"")); break;
        case '\\': escaped.addAll(kj::StringPtr("\\\\")); break;
        case '/' : escaped.addAll(kj::StringPtr("\\/" )); break;
        case '\b': escaped.addAll(kj::StringPtr("\\b")); break;
        case '\f': escaped.addAll(kj::StringPtr("\\f")); break;
        case '\n': escaped.addAll(kj::StringPtr("\\n")); break;
        case '\r': escaped.addAll(kj::StringPtr("\\r")); break;
        case '\t': escaped.addAll(kj::StringPtr("\\t")); break;
        default:
          if (c >= 0 && c < 0x20) {
            escaped.addAll(kj::StringPtr("\\u00"));
            uint8_t c2 = c;
            escaped.add(HEXDIGITS[c2 / 16]);
            escaped.add(HEXDIGITS[c2 % 16]);
          } else {
            escaped.add(c);
          }
          break;
      }
    }
    escaped.add('"');
    escaped.add('\0');

    return kj::String(escaped.releaseAsArray());
  }

  kj::StringTree encodeList(kj::Array<kj::StringTree> elements,
                            bool hasMultilineElement, uint indent, bool& multiline,
                            bool hasPrefix) const {
    size_t maxChildSize = 0;
    for (auto& e: elements) maxChildSize = kj::max(maxChildSize, e.size());

    kj::StringPtr prefix;
    kj::StringPtr delim;
    kj::StringPtr suffix;
    kj::String ownPrefix;
    kj::String ownDelim;
    if (!prettyPrint) {
      // No whitespace.
      delim = ",";
      prefix = "";
      suffix = "";
    } else if ((elements.size() > 1) && (hasMultilineElement || maxChildSize > 50)) {
      // If the array contained any multi-line elements, OR it contained sufficiently long
      // elements, then put each element on its own line.
      auto indentSpace = kj::repeat(' ', (indent + 1) * 2);
      delim = ownDelim = kj::str(",\n", indentSpace);
      multiline = true;
      if (hasPrefix) {
        // We're producing a multi-line list, and the first line has some garbage in front of it.
        // Therefore, move the first element to the next line.
        prefix = ownPrefix = kj::str("\n", indentSpace);
      } else {
        prefix = " ";
      }
      suffix = " ";
    } else {
      // Put everything on one line, but add spacing between elements for legibility.
      delim = ", ";
      prefix = "";
      suffix = "";
    }

    return kj::strTree(prefix, kj::StringTree(kj::mv(elements), delim), suffix);
  }
};

JsonCodec::JsonCodec()
    : impl(kj::heap<Impl>()) {}
JsonCodec::~JsonCodec() noexcept(false) {}

void JsonCodec::setPrettyPrint(bool enabled) { impl->prettyPrint = enabled; }

kj::String JsonCodec::encode(DynamicValue::Reader value, Type type) const {
  MallocMessageBuilder message;
  auto json = message.getRoot<JsonValue>();
  encode(value, type, json);
  return encodeRaw(json);
}

void JsonCodec::decode(kj::ArrayPtr<const char> input, DynamicStruct::Builder output) const {
  MallocMessageBuilder message;
  auto json = message.getRoot<JsonValue>();
  decodeRaw(input, json);
  decode(json, output);
}

Orphan<DynamicValue> JsonCodec::decode(
    kj::ArrayPtr<const char> input, Type type, Orphanage orphanage) const {
  MallocMessageBuilder message;
  auto json = message.getRoot<JsonValue>();
  decodeRaw(input, json);
  return decode(json, type, orphanage);
}

kj::String JsonCodec::encodeRaw(JsonValue::Reader value) const {
  bool multiline = false;
  return impl->encodeRaw(value, 0, multiline, false).flatten();
}

void JsonCodec::decodeRaw(kj::ArrayPtr<const char> input, JsonValue::Builder output) const {
  _::Parser parser(input);
  parser.parse(output);
}

void JsonCodec::encode(DynamicValue::Reader input, Type type, JsonValue::Builder output) const {
  // TODO(soon): For interfaces, check for handlers on superclasses, per documentation...
  // TODO(soon): For branded types, should we check for handlers on the generic?
  auto iter = impl->typeHandlers.find(type);
  if (iter != impl->typeHandlers.end()) {
    iter->second->encodeBase(*this, input, output);
    return;
  }

  switch (type.which()) {
    case schema::Type::VOID:
      output.setNull();
      break;
    case schema::Type::BOOL:
      output.setBoolean(input.as<bool>());
      break;
    case schema::Type::INT8:
    case schema::Type::INT16:
    case schema::Type::INT32:
    case schema::Type::UINT8:
    case schema::Type::UINT16:
    case schema::Type::UINT32:
    case schema::Type::FLOAT32:
    case schema::Type::FLOAT64:
      output.setNumber(input.as<double>());
      break;
    case schema::Type::INT64:
      output.setString(kj::str(input.as<int64_t>()));
      break;
    case schema::Type::UINT64:
      output.setString(kj::str(input.as<uint64_t>()));
      break;
    case schema::Type::TEXT:
      output.setString(kj::str(input.as<Text>()));
      break;
    case schema::Type::DATA: {
      // Turn into array of byte values. Yep, this is pretty ugly. People really need to override
      // this with a handler.
      auto bytes = input.as<Data>();
      auto array = output.initArray(bytes.size());
      for (auto i: kj::indices(bytes)) {
        array[i].setNumber(bytes[i]);
      }
      break;
    }
    case schema::Type::LIST: {
      auto list = input.as<DynamicList>();
      auto elementType = type.asList().getElementType();
      auto array = output.initArray(list.size());
      for (auto i: kj::indices(list)) {
        encode(list[i], elementType, array[i]);
      }
      break;
    }
    case schema::Type::ENUM: {
      auto e = input.as<DynamicEnum>();
      KJ_IF_MAYBE(symbol, e.getEnumerant()) {
        output.setString(symbol->getProto().getName());
      } else {
        output.setNumber(e.getRaw());
      }
      break;
    }
    case schema::Type::STRUCT: {
      auto structValue = input.as<capnp::DynamicStruct>();
      auto nonUnionFields = structValue.getSchema().getNonUnionFields();

      KJ_STACK_ARRAY(bool, hasField, nonUnionFields.size(), 32, 128);

      uint fieldCount = 0;
      for (auto i: kj::indices(nonUnionFields)) {
        fieldCount += (hasField[i] = structValue.has(nonUnionFields[i]));
      }

      // We try to write the union field, if any, in proper order with the rest.
      auto which = structValue.which();
      bool unionFieldIsNull = false;

      KJ_IF_MAYBE(field, which) {
        // Even if the union field is null, if it is not the default field of the union then we
        // have to print it anyway.
        unionFieldIsNull = !structValue.has(*field);
        if (field->getProto().getDiscriminantValue() != 0 || !unionFieldIsNull) {
          ++fieldCount;
        } else {
          which = nullptr;
        }
      }

      auto object = output.initObject(fieldCount);

      size_t pos = 0;
      for (auto i: kj::indices(nonUnionFields)) {
        auto field = nonUnionFields[i];
        KJ_IF_MAYBE(unionField, which) {
          if (unionField->getIndex() < field.getIndex()) {
            auto outField = object[pos++];
            outField.setName(unionField->getProto().getName());
            if (unionFieldIsNull) {
              outField.initValue().setNull();
            } else {
              encodeField(*unionField, structValue.get(*unionField), outField.initValue());
            }
            which = nullptr;
          }
        }
        if (hasField[i]) {
          auto outField = object[pos++];
          outField.setName(field.getProto().getName());
          encodeField(field, structValue.get(field), outField.initValue());
        }
      }
      if (which != nullptr) {
        // Union field not printed yet; must be last.
        auto unionField = KJ_ASSERT_NONNULL(which);
        auto outField = object[pos++];
        outField.setName(unionField.getProto().getName());
        if (unionFieldIsNull) {
          outField.initValue().setNull();
        } else {
          encodeField(unionField, structValue.get(unionField), outField.initValue());
        }
      }
      KJ_ASSERT(pos == fieldCount);
      break;
    }
    case schema::Type::INTERFACE:
      KJ_FAIL_REQUIRE("don't know how to JSON-encode capabilities; "
                      "please register a JsonCodec::Handler for this");
    case schema::Type::ANY_POINTER:
      KJ_FAIL_REQUIRE("don't know how to JSON-encode AnyPointer; "
                      "please register a JsonCodec::Handler for this");
  }
}

void JsonCodec::encodeField(StructSchema::Field field, DynamicValue::Reader input,
                            JsonValue::Builder output) const {
  auto iter = impl->fieldHandlers.find(field);
  if (iter != impl->fieldHandlers.end()) {
    iter->second->encodeBase(*this, input, output);
    return;
  }

  encode(input, field.getType(), output);
}

void JsonCodec::decode(JsonValue::Reader input, DynamicStruct::Builder output) const {
  KJ_FAIL_ASSERT("JSON decode not implement yet. :(");
}

Orphan<DynamicValue> JsonCodec::decode(
    JsonValue::Reader input, Type type, Orphanage orphanage) const {
  KJ_FAIL_ASSERT("JSON decode not implement yet. :(");
}

// -----------------------------------------------------------------------------

Orphan<DynamicValue> JsonCodec::HandlerBase::decodeBase(
    const JsonCodec& codec, JsonValue::Reader input, Orphanage orphanage) const {
  KJ_FAIL_ASSERT("JSON decoder handler type / value type mismatch");
}
void JsonCodec::HandlerBase::decodeStructBase(
    const JsonCodec& codec, JsonValue::Reader input, DynamicStruct::Builder output) const {
  KJ_FAIL_ASSERT("JSON decoder handler type / value type mismatch");
}

void JsonCodec::addTypeHandlerImpl(Type type, HandlerBase& handler) {
  impl->typeHandlers[type] = &handler;
}

void JsonCodec::addFieldHandlerImpl(StructSchema::Field field, Type type, HandlerBase& handler) {
  KJ_REQUIRE(type == field.getType(),
      "handler type did not match field type for addFieldHandler()");
  impl->fieldHandlers[field] = &handler;
}

} // namespace capnp
