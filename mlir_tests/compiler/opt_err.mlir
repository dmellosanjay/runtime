// Copyright 2021 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// RUN: tfrt_opt -split-input-file -verify-diagnostics %s

func @invalid_cost(%arg : i32) {
  %ch0 = tfrt.new.chain
  // expected-error @+1 {{requires the cost attribute larger than 0}}
  %ch1 = tfrt_test.test_cost %ch0 {id = 0 : i64, _tfrt_cost = 0 : i64}
  tfrt.return %ch1 : !tfrt.chain
}

// -----
