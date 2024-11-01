// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/common/common.h"
#include "core/common/status.h"
#include "core/common/path_string.h"
#include "core/framework/data_types.h"
#include "core/graph/model.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/session_state.h"
#include "test/test_environment.h"
#include "test_utils.h"
#include "test/util/include/asserts.h"

#include "gtest/gtest.h"

using namespace ONNX_NAMESPACE;
using namespace onnxruntime;

namespace onnxruntime {
namespace test {

std::vector<std::string> split(const std::string& str, char delimiter) {
  std::vector<std::string> result;
  std::stringstream ss(str);
  std::string token;

  // Use getline with a delimiter to split the string
  while (std::getline(ss, token, delimiter)) {
    result.push_back(token);
  }

  return result;
}

Status LoadSaveAndCompareModel(const std::filesystem::path& input_onnx,
                               const std::filesystem::path& input_external_init_file,
                               const std::filesystem::path& output_onnx,
                               const std::filesystem::path& output_external_init_file,
                               size_t initializer_size_threshold,
                               const Graph::OffsetAlignmentInfo& align_info,
                               Graph::PrePackedTensorProtoToSave& pre_packed_initializers_tensor_proto,
                               bool save_prepacked_constant_initializers = false) {
  auto logger = DefaultLoggingManager().CreateLogger("LoadSaveAndCompareModel");
  std::shared_ptr<Model> model;
  ORT_RETURN_IF_ERROR(Model::Load(input_onnx, model, nullptr, *logger));
  std::filesystem::remove(output_onnx);
  std::filesystem::remove(output_external_init_file);
  ORT_RETURN_IF_ERROR(Model::SaveWithExternalInitializers(*model, output_onnx, output_external_init_file, initializer_size_threshold,
                                                          align_info, save_prepacked_constant_initializers, pre_packed_initializers_tensor_proto));

  std::shared_ptr<Model> model_from_external;
  ORT_RETURN_IF_ERROR(Model::Load(output_onnx.native(), model_from_external, nullptr, *logger));

  Graph& graph = model->MainGraph();
  // Perform shape inference on the graph, if this succeeds then it means that we could correctly read the
  // integer initializers used by reshape and transpose.
  ORT_RETURN_IF_ERROR(graph.Resolve());
  Graph& graph_from_external = model_from_external->MainGraph();

  InitializedTensorSet initializers = graph.GetAllInitializedTensors();
  InitializedTensorSet initializers_from_external = graph_from_external.GetAllInitializedTensors();

  ORT_RETURN_IF_NOT(initializers.size() == initializers_from_external.size(), "size mismatch");

  // Compare the initializers of the two versions.
  std::filesystem::path model_path{};
  std::filesystem::path external_data_path{};
  for (const auto& i : initializers_from_external) {
    const std::string kInitName = i.first;
    const ONNX_NAMESPACE::TensorProto* from_external_tensor_proto = i.second;
    // prepack initializer will have name as [original name]:[kernel name] in case initializer used by multiple kernels
    const ONNX_NAMESPACE::TensorProto* tensor_proto = save_prepacked_constant_initializers ? initializers[split(kInitName, ':')[0]] : initializers[kInitName];

    std::vector<uint8_t> tensor_proto_data;
    model_path = input_onnx;
    external_data_path = (!input_external_init_file.empty()) ? (model_path.parent_path() / input_external_init_file) : std::filesystem::path();
    ORT_RETURN_IF_ERROR(utils::UnpackInitializerData(*tensor_proto, external_data_path, tensor_proto_data));
    size_t tensor_proto_size = tensor_proto_data.size();

    std::vector<uint8_t> from_external_tensor_proto_data;
    model_path = output_onnx;
    external_data_path = model_path.parent_path() / output_external_init_file;
    ORT_RETURN_IF_ERROR(utils::UnpackInitializerData(*from_external_tensor_proto, model_path, from_external_tensor_proto_data));
    size_t from_external_tensor_proto_size = from_external_tensor_proto_data.size();

    if (from_external_tensor_proto_size < initializer_size_threshold) {
      // 'Small' tensors should be embedded in the onnx file.
      ORT_RETURN_IF_NOT(from_external_tensor_proto->data_location() == ONNX_NAMESPACE::TensorProto_DataLocation::TensorProto_DataLocation_DEFAULT, "location mismatch");
    } else {
      // 'Large' tensors should be added to the external binary file.
      ORT_RETURN_IF_NOT(from_external_tensor_proto->data_location() == ONNX_NAMESPACE::TensorProto_DataLocation::TensorProto_DataLocation_EXTERNAL, "location mismatch");
    }

    if (!save_prepacked_constant_initializers) {
      ORT_RETURN_IF_NOT(tensor_proto_size == from_external_tensor_proto_size, "size mismatch");
      ORT_RETURN_IF_NOT(memcmp(tensor_proto_data.data(), from_external_tensor_proto_data.data(), tensor_proto_size) == 0, "data mismatch");
    } else {
      ORT_RETURN_IF_NOT(from_external_tensor_proto_size >= tensor_proto_size, "prepack initializer's size is at least same as original tensor, might be larger");
    }

    if (align_info.align_offset) {
      for (const StringStringEntryProto& entry : from_external_tensor_proto->external_data()) {
        if (entry.has_key() && entry.has_value() && entry.key() == "offset") {
          size_t tensor_offset;
          std::stringstream stream(entry.value());
          stream >> tensor_offset;
          ORT_RETURN_IF_NOT(tensor_offset % align_info.allocation_granularity == 0, "tensor offset not align");
        }
      }
    }
  }

  // Cleanup.
  ORT_RETURN_IF_NOT(std::filesystem::remove(output_onnx), "delete file failed");
  ORT_RETURN_IF_NOT(std::filesystem::remove(external_data_path), "delete file failed");
  return Status::OK();
}

// Original model does not have external initializers
TEST(SaveWithExternalInitializers, Mnist) {
  Graph::OffsetAlignmentInfo align_info;
  Graph::PrePackedTensorProtoToSave pre_packed_initializers_tensor_proto;
  ASSERT_STATUS_OK(LoadSaveAndCompareModel(ORT_TSTR("testdata/mnist.onnx"), ORT_TSTR(""), ORT_TSTR("testdata/mnist_with_external_initializers.onnx"), ORT_TSTR("mnist_external_initializers.bin"), 100, align_info, pre_packed_initializers_tensor_proto));
}

// Original model has external initializers
TEST(SaveWithExternalInitializers, ModelWithOriginalExternalData) {
  Graph::OffsetAlignmentInfo align_info;
  Graph::PrePackedTensorProtoToSave pre_packed_initializers_tensor_proto;
  ASSERT_STATUS_OK(LoadSaveAndCompareModel(ORT_TSTR("testdata/model_with_orig_ext_data.onnx"), ORT_TSTR("model_with_orig_ext_data.onnx.data"), ORT_TSTR("testdata/model_with_new_external_initializers.onnx"), ORT_TSTR("model_with_new_external_initializers.bin"), 0, align_info, pre_packed_initializers_tensor_proto));
}

// Original model has external initializers, align offset
TEST(SaveWithExternalInitializers, ModelWithOriginalExternalDataAlignOffset) {
  Graph::OffsetAlignmentInfo align_info;
  align_info.align_offset = true;
  align_info.align_threshold = 0;
  Graph::PrePackedTensorProtoToSave pre_packed_initializers_tensor_proto;
  ASSERT_STATUS_OK(LoadSaveAndCompareModel(ORT_TSTR("testdata/model_with_orig_ext_data.onnx"), ORT_TSTR("model_with_orig_ext_data.onnx.data"), ORT_TSTR("testdata/model_with_new_external_initializers.onnx"), ORT_TSTR("model_with_new_external_initializers.bin"), 0, align_info, pre_packed_initializers_tensor_proto));
}

// Original model has external initializers, align offset and serialize prepacked external initializer to model file
TEST(SaveWithExternalInitializers, ModelWithOriginalExternalDataAlignOffsetAndSavePrepackTensors) {
  Graph::OffsetAlignmentInfo align_info;
  align_info.align_offset = true;
  align_info.align_threshold = 0;
  std::shared_ptr<CPUAllocator> alloc = std::make_shared<CPUAllocator>();
  TensorShape shape = {178};
  // prepack both initializers for test purpose
  Graph::PrePackedTensorProtoToSave pre_packed_initializers_tensor_proto;
  pre_packed_initializers_tensor_proto["MatMul.Weight"]["MatMul_0"] = utils::TensorToTensorProto(Tensor(DataTypeImpl::GetType<uint8_t>(), shape, alloc), "MatMul.Weight:MatMul_0");
  pre_packed_initializers_tensor_proto["scales"]["MatMul_0"] = utils::TensorToTensorProto(Tensor(DataTypeImpl::GetType<float>(), shape, alloc), "scales:MatMul_0");
  ASSERT_STATUS_OK(LoadSaveAndCompareModel(ORT_TSTR("testdata/prepack/model_with_matmul_nbits.onnx"), ORT_TSTR("model_with_matmul_nbits.onnx.data"), ORT_TSTR("testdata/prepack/model_with_matmul_nbits_opt.onnx"), ORT_TSTR("model_with_matmul_nbits_opt.onnx.data"), 0, align_info, pre_packed_initializers_tensor_proto, true));
}

}  // namespace test
}  // namespace onnxruntime
