// Copyright 2020 The TensorFlow Runtime Authors
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

// RUN: bef_executor_lite $(bef_name %s) --work_queue_type=mstd 2>&1 | FileCheck %s --dump-input=fail

module attributes {tfrt.cost_threshold = 10 : i64} {

// CHECK-LABEL: --- Running 'print_thread_id'
func @print_thread_id() -> !tfrt.chain {
  %ch = tfrt.new.chain
  %t = "tfrt_test.get_thread_id"(%ch) : (!tfrt.chain) -> (i32)

  // CHECK: int32 = 0
  %ch0 = tfrt.print.i32 %t, %ch

  %t0 = "tfrt_test.get_thread_id"(%ch0) : (!tfrt.chain) -> (i32)
  %t1 = "tfrt_test.get_thread_id"(%ch0) : (!tfrt.chain) -> (i32)

  // CHECK: int32 = 0
  // CHECK: int32 = 1
  %ch1 = tfrt.print.i32 %t0, %ch0
  %ch2 = tfrt.print.i32 %t1, %ch1

  %ch3 = tfrt.merge.chains %ch1, %ch2

  tfrt.return %ch3 : !tfrt.chain
}

// CHECK-LABEL: --- Running 'breadth_first'
func @breadth_first() -> !tfrt.chain {
  %ch0 = tfrt.new.chain

  // The kernels of id 0 and id 1 should be executed before the kernel of id 2
  // as id 0 and id 1 are in the same level while id 2 is one level deeper.
  // This is to enqueue parallel sequences as early as possible.

  // CHECK: id: {{[01]}}
  // CHECK: id: {{[01]}}
  // CHECK: id: 2
  %ch1 = tfrt_test.test_cost %ch0 {id = 0 : i64, _tfrt_cost = 1 : i64}
  %ch2 = tfrt_test.test_cost %ch0 {id = 1 : i64, _tfrt_cost = 1 : i64}
  %ch3 = tfrt_test.test_cost %ch1 {id = 2 : i64, _tfrt_cost = 100 : i64}

  %ch4 = tfrt.merge.chains %ch2, %ch3
  tfrt.return %ch4: !tfrt.chain
}

}
