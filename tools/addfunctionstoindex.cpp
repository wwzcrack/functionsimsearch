// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fstream>
#include <iostream>
#include <map>
#include <gflags/gflags.h>

#include "CodeObject.h"
#include "InstructionDecoder.h"

#include "disassembly/disassembly.hpp"
#include "disassembly/dyninstfeaturegenerator.hpp"
#include "disassembly/flowgraph.hpp"
#include "disassembly/flowgraphutil.hpp"
#include "searchbackend/functionsimhash.hpp"
#include "searchbackend/simhashsearchindex.hpp"
#include "disassembly/pecodesource.hpp"
#include "util/threadpool.hpp"
#include "util/util.hpp"

DEFINE_string(format, "PE", "Executable format: PE or ELF");
DEFINE_string(input, "", "File to disassemble");
DEFINE_string(index, "./similarity.index", "Index file");
DEFINE_string(weights, "weights.txt", "Feature weights file");
DEFINE_uint64(minimum_function_size, 5, "Minimum size of a function to be added.");
DEFINE_bool(no_shared_blocks, false, "Skip functions with shared blocks.");

// The google namespace is there for compatibility with legacy gflags and will
// be removed eventually.
#ifndef gflags
using namespace google;
#else
using namespace gflags;
#endif

using namespace std;
using namespace Dyninst;
using namespace ParseAPI;
using namespace InstructionAPI;

int main(int argc, char** argv) {
  SetUsageMessage(
    "Add the functions of the input executable which exceed a certain minimum "
    "size to the search index specified.");
  ParseCommandLineFlags(&argc, &argv, true);

  std::string mode(FLAGS_format);
  std::string binary_path_string(FLAGS_input);
  std::string index_file(FLAGS_index);
  uint64_t minimum_size = FLAGS_minimum_function_size;

  if (binary_path_string == "") {
    printf("[!] Empty target binary.\n");
    return -1;
  }
  uint64_t file_id = GenerateExecutableID(binary_path_string);
  printf("[!] Executable id is %16.16lx\n", file_id);

  // Load the search index.
  SimHashSearchIndex search_index(index_file, false);

  Disassembly disassembly(mode, binary_path_string);
  if (!disassembly.Load()) {
    exit(1);
  }
  CodeObject* code_object = disassembly.getCodeObject();

  // Obtain the list of all functions in the binary.
  const CodeObject::funclist &functions = code_object->funcs();
  if (functions.size() == 0) {
    printf("No functions found.\n");
    return -1;
  }

  std::mutex search_index_mutex;
  std::mutex* mutex_pointer = &search_index_mutex;
  threadpool::ThreadPool pool(std::thread::hardware_concurrency());
  std::atomic_ulong atomic_processed_functions(0);
  std::atomic_ulong* processed_functions = &atomic_processed_functions;
  uint64_t number_of_functions = functions.size();
  FunctionSimHasher hasher(FLAGS_weights);

  for (Function* function : functions) {
    // Skip functions that contain shared basic blocks.
    if (FLAGS_no_shared_blocks && ContainsSharedBasicBlocks(function)) {
      continue;
    }

    pool.Push(
      [&search_index, mutex_pointer, &binary_path_string, &hasher,
      processed_functions, file_id, function, minimum_size,
      number_of_functions](int threadid) {
      Flowgraph graph;
      Address function_address = function->addr();
      BuildFlowgraph(function, &graph);
      (*processed_functions)++;

      uint64_t branching_nodes = graph.GetNumberOfBranchingNodes();

      if (branching_nodes <= minimum_size) {
        printf("[!] (%lu/%lu) %s FileID %lx: Skipping function %lx, only %lu "
          "branching nodes\n", processed_functions->load(), number_of_functions,
          binary_path_string.c_str(), file_id, function_address, branching_nodes);
        return;
      }
      if (search_index.GetIndexFileFreeSpace() < (1ULL << 14)) {
        printf("[!] (%lu/%lu) %s FileID %lx: Skipping function %lx. Index file "
          "full.\n", processed_functions->load(), number_of_functions,
          binary_path_string.c_str(), file_id, function_address);
        return;
      }

      printf("[!] (%lu/%lu) %s FileID %lx: Adding function %lx (%lu branching "
        "nodes)\n", processed_functions->load(), number_of_functions,
        binary_path_string.c_str(), file_id, function_address, branching_nodes);

      std::vector<uint64_t> hashes;
      // Access to the DynInst API which happens inside the constructor of the
      // generator needs to be synchronized.
      mutex_pointer->lock();
      DyninstFeatureGenerator generator(function);
      mutex_pointer->unlock();

      hasher.CalculateFunctionSimHash(&generator, 128, &hashes);
      uint64_t hash_A = hashes[0];
      uint64_t hash_B = hashes[1];
      {
        std::lock_guard<std::mutex> lock(*mutex_pointer);
        try {
          search_index.AddFunction(hash_A, hash_B, file_id, function_address);
        } catch (boost::interprocess::bad_alloc& out_of_space) {
          printf("[!] boost::interprocess::bad_alloc - no space in index file "
            "left!\n");
        }
      }
    });
  }
  pool.Stop(true);
}
