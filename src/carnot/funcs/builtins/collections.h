/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "src/carnot/udf/registry.h"
#include "src/shared/types/types.h"

namespace px {
namespace carnot {
namespace builtins {
/**
 * Registers UDF operations that work on collections
 * @param registry pointer to the registry.
 */
void RegisterCollectionOpsOrDie(udf::Registry* registry);

template <typename T>
types::StringValue SerializeScalar(T* value) {
  return types::StringValue(reinterpret_cast<char*>(value), sizeof(*value));
}

template <>
inline types::StringValue SerializeScalar(types::StringValue* value) {
  return *value;
}

template <typename T>
T DeserializeScalar(const types::StringValue& data) {
  return *reinterpret_cast<const typename types::ValueTypeTraits<T>::native_type*>(data.data());
}

template <>
inline types::StringValue DeserializeScalar(const types::StringValue& data) {
  return data;
}

template <typename TArg>
class AnyUDA : public udf::UDA {
 public:
  AnyUDA() = default;
  void Update(FunctionContext*, TArg val) {
    // TODO(zasgar): We should find a way to short-circuit the agg since we only care
    // about one value.
    if (!picked) {
      val_ = val;
      picked = true;
    }
  }

  void Merge(FunctionContext*, const AnyUDA&) {
    // Does nothing.
  }

  TArg Finalize(FunctionContext*) { return val_; }

  static udf::InfRuleVec SemanticInferenceRules() {
    return {udf::InheritTypeFromArgs<AnyUDA>::CreateGeneric()};
  }

  StringValue Serialize(FunctionContext*) { return SerializeScalar(&val_); }

  Status Deserialize(FunctionContext*, const StringValue& data) {
    val_ = DeserializeScalar<TArg>(data);
    return Status::OK();
  }

  static udf::UDADocBuilder Doc() {
    return udf::UDADocBuilder("Picks any single value.")
        .Details("Picks a value from the collection. No guarantees on which value is picked.")
        .Example(R"doc(
        | # Calculate any value from the collection.
        | df = df.agg(latency_dist=('val', px.any))
        )doc")
        .Arg("val", "The data to select the value from.")
        .Returns("The a single record selected from the above val.");
  }

 protected:
  TArg val_;
  bool picked = false;
};

}  // namespace builtins
}  // namespace carnot
}  // namespace px
