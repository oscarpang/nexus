#ifdef USE_CAFFE

#include <boost/filesystem.hpp>
#include <boost/make_shared.hpp>
#include <fstream>
#include <glog/logging.h>
#include <opencv2/opencv.hpp>
#include <sstream>

#include "nexus/backend/caffe_model.h"
#include "nexus/backend/postprocess.h"
#include "nexus/backend/slice.h"
#include "nexus/common/image.h"
#include "nexus/common/util.h"
#include "nexus/proto/control.pb.h"

namespace fs = boost::filesystem;

namespace nexus {
namespace backend {

CaffeModel::CaffeModel(int gpu_id, const ModelInstanceConfig& config,
                       const YAML::Node& info) :
    ModelInstance(gpu_id, config, info) {
  CHECK(info["cfg_file"]) << "Missing cfg_file in the model info";
  CHECK(info["weight_file"]) << "Missing weight_file in the model info";
  CHECK(info["mean_file"] || info["mean_value"])
      << "Missing mean_file or mean_value in the model info";
  // load caffe model
  fs::path model_dir = fs::path(info["model_dir"].as<std::string>());
  fs::path cfg_path = model_dir / info["cfg_file"].as<std::string>();
  fs::path weight_path = model_dir / info["weight_file"].as<std::string>();
  CHECK(fs::exists(cfg_path)) << "cfg file " << cfg_path <<
      " doesn't exist";
  CHECK(fs::exists(weight_path)) << "weight file " << weight_path <<
      " doesn't exist";

  // init gpu device
  caffe::Caffe::SetDevice(gpu_id);
  caffe::Caffe::set_mode(caffe::Caffe::GPU);

  // load network
  net_.reset(new caffe::ServeNet<float>(cfg_path.string(), max_batch_));
  net_->CopyTrainedLayersFrom(weight_path.string());
  // get input and output shape
  // NOTE: currently we only consider single input and single output
  CHECK_EQ(net_->num_inputs(), 1)
      << "CaffeModel only support caffe model that has single input";
  CHECK_EQ(net_->num_outputs(), 1)
      << "CaffeModel only support caffe model that has single output";
  input_shape_ = std::vector<int>(net_->input_blobs()[0]->shape());
  output_shape_ = std::vector<int>(net_->output_blobs()[0]->shape());
  input_blob_idx_ = net_->input_blob_indices()[0];
  output_blob_name_ = net_->blob_names()[net_->output_blob_indices()[0]];

  // get the single input and output size
  input_size_ = 1;
  output_size_ = 1;
  for (size_t i = 1; i < input_shape_.size(); ++i) {
    input_size_ *= input_shape_[i];
  }
  for (size_t i = 1; i < output_shape_.size(); ++i) {
    output_size_ *= output_shape_[i];
  }
  image_height_ = input_shape_[2];
  image_width_ = input_shape_[3];
  LOG(INFO) << "Model " << model_session_id_ << ": input size " <<
      input_size_ << ", output size " << output_size_;
  
  // set up data transformer
  caffe::TransformationParameter transform_param;
  if (info["scale"]) {
    transform_param.set_scale(info["scale"].as<float>());
  }
  transform_param.set_crop_size(image_height_);
  if (info["mean_file"]) {
    fs::path mean_file = model_dir / info["mean_file"].as<std::string>();
    transform_param.set_mean_file(mean_file.string());
  } else {
    const YAML::Node& mean_values = info["mean_value"];
    CHECK(mean_values.IsSequence()) <<
        "mean_value in the config is not sequence";
    for (uint i = 0; i < mean_values.size(); ++i) {
      transform_param.add_mean_value(mean_values[i].as<float>());
    }
  }
  transformer_.reset(new caffe::DataTransformer<float>(
      transform_param, net_->phase()));

  // whether enbable prefix batching
  if (info["prefix_layer"]) {
    prefix_layer_ = info["prefix_layer"].as<std::string>();
    prefix_index_ = net_->layer_index_by_name(prefix_layer_);
    LOG(INFO) << "Prefix layer up to " << prefix_layer_ << "(" <<
        prefix_index_ << ")";
  } else {
    prefix_layer_ = "";
    prefix_index_ = -1;
  }
  // load classnames
  if (info["class_names"]) {
    fs::path cns_path = model_dir / info["class_names"].as<std::string>();
    LoadClassnames(cns_path.string());
  }
}

ArrayPtr CaffeModel::CreateInputGpuArray() {
  boost::shared_ptr<caffe::Blob<float> > blob;
  if (input_blobs_.empty()) {
    blob = net_->blobs()[input_blob_idx_];
  } else {
    blob = boost::make_shared<caffe::Blob<float> >(input_shape_);
  }
  auto buf = std::make_shared<Buffer>(blob->mutable_gpu_data(),
                                      blob->count() * sizeof(float),
                                      gpu_device_);
  auto arr = std::make_shared<Array>(DT_FLOAT, blob->count(), buf);
  arr->set_tag(input_blobs_.size());
  input_blobs_.push_back(blob);
  return arr;
}

std::unordered_map<std::string, size_t> CaffeModel::OutputSizes() const {
  return {{output_blob_name_, output_size_}};
}

void CaffeModel::Preprocess(std::shared_ptr<Task> task) {
  auto prepare_image = [&](cv::Mat& image) {
    auto in_arr = std::make_shared<Array>(DT_FLOAT, input_size_, cpu_device_);
    cv::Mat resized_image;
    cv::resize(image, resized_image, cv::Size(image_width_, image_height_));
    std::vector<int> blob_shape = input_shape_;
    blob_shape[0] = 1;
    caffe::Blob<float> blob(blob_shape);
    blob.data()->set_cpu_data(in_arr->Data<void>());
    transformer_->Transform(resized_image, &blob);
    task->AppendInput(in_arr);
  };

  const auto& query = task->query;
  const auto& input_data = query.input();
  switch (input_data.data_type()) {
    case DT_IMAGE: {
      cv::Mat cv_img_bgr = DecodeImage(input_data.image(), CO_BGR);
      if (query.window_size() > 0) {
        for (int i = 0; i < query.window_size(); ++i) {
          const auto& rect = query.window(i);
          cv::Mat crop_img = cv_img_bgr(cv::Rect(
              rect.left(), rect.top(), rect.right() - rect.left(),
              rect.bottom() - rect.top()));
          prepare_image(crop_img);
        }
      } else {
        prepare_image(cv_img_bgr);
      }
      break;
    }
    default:
      task->result.set_status(INPUT_TYPE_INCORRECT);
      task->result.set_error_message("Input type incorrect: " +
                                     DataType_Name(input_data.data_type()));
      break;
  }
}

void CaffeModel::Forward(BatchInput* batch_input, BatchOutput* batch_output) {
  auto blob = input_blobs_[batch_input->array()->tag()];
  // reshape input blob to current batch size
  size_t batch = batch_input->batch_size();
  std::vector<int> input_shape = input_shape_;
  input_shape[0] = batch;
  blob->Reshape(input_shape);
  // Replace input blob in the network by the corresponding blob
  net_->set_blob(input_blob_idx_, blob);
  auto out_arr = batch_output->GetArray(output_blob_name_);
  // We don't need to reshape the network, because during the forwarding
  // Caffe will reshape every layers based on the input batch size
  if (prefix_index_ >= 0) {
#if 1
    // Forward the shared prefix part
    net_->ForwardTo(prefix_index_);
    auto prefix_top_blob_id = net_->top_ids(prefix_index_)[0];
    auto prefix_top_blob = net_->blobs()[prefix_top_blob_id];
    float* prefix_top_gpu_data = prefix_top_blob->mutable_gpu_data();
    std::vector<int> bottom_shape = prefix_top_blob->shape();
    bottom_shape[0] = 1; // change to single batch
    boost::shared_ptr<caffe::Blob<float> > split_bottom_blob(
        new caffe::Blob<float>(bottom_shape));
    int split_count = split_bottom_blob->count();
    std::vector<int> blob_shape = input_shape_;
    // Forward the split part
    for (size_t i = 0; i < batch; ++i) {
      split_bottom_blob->set_gpu_data(prefix_top_gpu_data + i * split_count);
      net_->set_blob(prefix_top_blob_id, split_bottom_blob);
      net_->ForwardFrom(prefix_index_ + 1);
      const caffe::Blob<float>* output_blob = net_->output_blobs()[0];
      Memcpy(out_arr->Data<void>() + i * output_size_, cpu_device_,
             output_blob->gpu_data(), gpu_device_, output_size_ * sizeof(float));
    }
    // restore prefix top blob
    net_->set_blob(prefix_top_blob_id, prefix_top_blob);
#else
    // No prefix batching
    auto input_blob = net_->blobs()[0];
    float* gpu_data = input_blob->mutable_gpu_data();
    input_shape[0] = 1;
    boost::shared_ptr<caffe::Blob<float> > single_input_blob(
        new caffe::Blob<float>(input_shape));
    for (size_t i = 0; i < batch; ++i) {
      single_input_blob->set_gpu_data(gpu_data + i * input_size_);
      net_->set_blob(0, single_input_blob);
      auto output_blob = net_->Forward()[0];
      Memcpy(out_arr->Data<void>() + i * output_size_, cpu_device_,
             output_blob->gpu_data(), gpu_device_, output_size_ * sizeof(float));
    }
    net_->set_blob(0, input_blob);
#endif
  } else {
    const caffe::Blob<float>* output_blob = net_->Forward()[0];
    Memcpy(out_arr->Data<void>(), cpu_device_, output_blob->gpu_data(),
           gpu_device_, output_blob->count() * sizeof(float));
  }
  batch_output->SliceBatch({{output_blob_name_, Slice(batch, output_size_)}});
}

void CaffeModel::Postprocess(std::shared_ptr<Task> task) {
  const QueryProto& query = task->query;
  QueryResultProto* result = &task->result;
  result->set_status(CTRL_OK);
  for (auto& output : task->outputs) {
    auto out_arr = output->GetArray(output_blob_name_);
    float* out_data = out_arr->Data<float>();
    if (type_ == "classification") {
      if (classnames_.empty()) {
        PostprocessClassification(query, out_data, output_size_, result);
      } else {
        PostprocessClassification(query, out_data, output_size_, result,
                                  &classnames_);
      }
    } else {
      std::ostringstream oss;
      oss << "Unsupported model type " << type() << " for " << framework();
      result->set_status(MODEL_TYPE_NOT_SUPPORT);
      result->set_error_message(oss.str());
      break;
    }
  }
}

void CaffeModel::LoadClassnames(const std::string& filepath) {
  std::ifstream fs(filepath);
  CHECK(fs.good()) << "Classname file " << filepath << " doesn't exist";
  std::string line;
  while (std::getline(fs, line)) {
    classnames_.push_back(line);
  }
  fs.close();
}

} // namespace backend
} // namespace nexus

#endif // USE_CAFFE
