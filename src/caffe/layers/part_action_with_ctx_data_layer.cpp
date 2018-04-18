#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/part_action_with_ctx_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
PartActionWithCtxDataLayer<Dtype>::~PartActionWithCtxDataLayer<Dtype>() {
  this->StopInternalThread();
}

template <typename Dtype>
void PartActionWithCtxDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.part_action_with_ctx_data_param().new_height();
  const int new_width  = this->layer_param_.part_action_with_ctx_data_param().new_width();
  const bool is_color  = this->layer_param_.part_action_with_ctx_data_param().is_color();
  
  const int label_bias = this->layer_param_.part_action_with_ctx_data_param().label_bias();

  string root_folder_bbox = this->layer_param_.part_action_with_ctx_data_param().root_folder_bbox();
  string root_folder_ctx = this->layer_param_.part_action_with_ctx_data_param().root_folder_ctx();
  string root_folder_part = this->layer_param_.part_action_with_ctx_data_param().root_folder_part();
  string root_folder_anno = this->layer_param_.part_action_with_ctx_data_param().root_folder_anno();


  /*vector<float> test_tmp= this->layer_param_.ctx_data_param().jit_scale();
  for(int c = 0; c < this->layer_param_.ctx_data_param().num_scales(); ++c){
    jit_size.push_back(test_tmp[c]);
    CHECK_GT(jit_size[c], 0) << "Jit scale must be more than 0!";
    CHECK_LE(jit_size[c], new_height) << "Jit scale must be less than image size!";
  }*/
  
  CHECK_GE(label_bias,0);

  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";
  // Read the file with filenames and labels
  const string& source = this->layer_param_.part_action_with_ctx_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  string line;
  size_t pos;
  int label;
  while (std::getline(infile, line)) {
    pos = line.find_last_of(' ');
    label = atoi(line.substr(pos + 1).c_str());
    lines_.push_back(std::make_pair(line.substr(0, pos), label));
  }

  CHECK(!lines_.empty()) << "File is empty";

  if (this->layer_param_.part_action_with_ctx_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.part_action_with_ctx_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.part_action_with_ctx_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }
  // Read an image, and use it to initialize the top blob.
  cv::Mat cv_img = ReadImageToCVMat(root_folder_bbox + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
  // Use data_transformer to infer the expected blob shape from a cv_image.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape prefetch_data and top[0] according to the batch_size.
  const int batch_size = this->layer_param_.part_action_with_ctx_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  //Zhichen
  top_shape[0] = batch_size*(2+7);// bbox+ctx+head+torso+legs+arms(2)+hands(2)
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  LOG(INFO) << "output data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();
  // label
  vector<int> label_shape(1, batch_size*(2+7));
  top[1]->Reshape(label_shape);
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].label_.Reshape(label_shape);
  }
}

template <typename Dtype>
void PartActionWithCtxDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is called on prefetch thread
template <typename Dtype>
void PartActionWithCtxDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  PartActionWithCtxDataParameter part_action_with_ctx_data_param = this->layer_param_.part_action_with_ctx_data_param();
  const int batch_size = part_action_with_ctx_data_param.batch_size();
  const int new_height = part_action_with_ctx_data_param.new_height();
  const int new_width = part_action_with_ctx_data_param.new_width();
  const bool is_color = part_action_with_ctx_data_param.is_color();

  const int label_bias = part_action_with_ctx_data_param.label_bias();
 
  string root_folder_bbox = part_action_with_ctx_data_param.root_folder_bbox();
  string root_folder_ctx = part_action_with_ctx_data_param.root_folder_ctx();
  string root_folder_part = part_action_with_ctx_data_param.root_folder_part();
  string root_folder_anno = part_action_with_ctx_data_param.root_folder_anno();  

  //int jit_num = this->data_transformer_->jit_size_.size();
  int jit_low = this->data_transformer_->jit_size_[0];
  int jit_upp = this->data_transformer_->jit_size_[1];
  //LOG(INFO) << jit_low << jit_upp;
  int part_kinds = 7;
  int all_kinds = part_kinds + 2;
  vector<string> part_names;
  part_names.push_back("head");
  part_names.push_back("torso");
  part_names.push_back("legs");
  part_names.push_back("larm");
  part_names.push_back("rarm");
  part_names.push_back("lhand");
  part_names.push_back("rhand");


  //vector<Dtype> jit_size = ctx_data_param.jit_scale(); 

  // Reshape according to the first image of each batch
  // on single input batches allows for inputs of varying dimension.
  cv::Mat cv_img = ReadImageToCVMat(root_folder_bbox + lines_[lines_id_].first,
      new_height, new_width, is_color);
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
  // Use data_transformer to infer the expected blob shape from a cv_img.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape batch according to the batch_size.
  // Zhichen
  top_shape[0] = batch_size*(2+7);
  batch->data_.Reshape(top_shape);

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();
  Dtype* prefetch_label = batch->label_.mutable_cpu_data();

  // datum scales
  const int lines_size = lines_.size();
  //Zhichen
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);

    int jit_idx = jit_low + this->data_transformer_->Rand(jit_upp-jit_low + 1);
    //int jit_idx = this->data_transformer_->Rand(jit_num);
    //int flip_idx = this->data_transformer_->Rand(2);

    //new_height, new_width
    cv::Mat cv_img_bbox = ReadImageToCVMat(root_folder_bbox + lines_[lines_id_].first,0, 0, is_color);
    CHECK(cv_img_bbox.data) << "Could not load " << lines_[lines_id_].first;
    cv::Mat cv_img_ctx = ReadImageToCVMat(root_folder_ctx + lines_[lines_id_].first,
        0, 0, is_color);
    CHECK(cv_img_ctx.data) << "Could not load " << lines_[lines_id_].first;
	
    


    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image

    vector<cv::Mat> cv_img_parts;
    vector<int> part_annos;

    std::ifstream part_anno_file;
    int tmp_anno;
    bool phase_train;
    string anno_path = root_folder_anno + lines_[lines_id_].first.substr(0,lines_[lines_id_].first.size()-4) + ".txt";   

    if(access(anno_path.c_str(), 0 )!=-1){
	phase_train = true;
	part_anno_file.open(anno_path.c_str());
    }
    else{
	phase_train = false;
    }

    cv::Mat cv_img_filler(new_height,new_width,CV_8UC3,cv::Scalar(128,128,128));
    for(int part_id = 0; part_id < part_names.size(); part_id++ ){
        string part_path = root_folder_part + lines_[lines_id_].first.substr(0,lines_[lines_id_].first.size()-4) + "_" + part_names[part_id] + ".jpg";
	if(access(part_path.c_str(), 0 )!=-1){	
	    cv::Mat cv_img_part = ReadImageToCVMat(part_path,
            0, 0, is_color);
	   
	    cv_img_parts.push_back(cv_img_part);
	    part_anno_file>>tmp_anno;//notice
	    if(tmp_anno!=-1 && phase_train){
	      
              CHECK_NE(tmp_anno, 0);
	      //LOG(INFO) <<tmp_anno + label_bias<<" ";
	      part_annos.push_back(tmp_anno + label_bias -  1);// add -1
	    }
	    else 
	      part_annos.push_back(lines_[lines_id_].second);
	}
	else{
	    cv_img_parts.push_back(cv_img_filler);
	    part_annos.push_back(74);
	}    
    }
    part_anno_file.close();

   
    int offset = batch->data_.offset(item_id*all_kinds);
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    this->data_transformer_->Transform(cv_img_bbox, &(this->transformed_data_), jit_idx);

    prefetch_label[item_id*all_kinds] = lines_[lines_id_].second;

    offset = batch->data_.offset(item_id*all_kinds+1);
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    //jit_idx = this->data_transformer_->Rand(jit_num);
    //jit_idx = jit_low + this->data_transformer_->Rand(jit_upp-jit_low);
    this->data_transformer_->Transform(cv_img_ctx, &(this->transformed_data_), jit_idx);

    prefetch_label[item_id*all_kinds+1] = lines_[lines_id_].second;
//changed
    for(int part_id = 0; part_id < part_kinds; part_id++){
	offset = batch->data_.offset(item_id*all_kinds+2+part_id);
    	this->transformed_data_.set_cpu_data(prefetch_data + offset);
        //jit_idx = this->data_transformer_->Rand(jit_num);
    	this->data_transformer_->Transform(cv_img_parts[part_id % part_kinds], &(this->transformed_data_), jit_idx);

	prefetch_label[item_id*all_kinds+2+part_id] = part_annos[part_id % part_kinds];
    }

    trans_time += timer.MicroSeconds();


    // Zhichen
    //prefetch_label[item_id] = lines_[lines_id_].second;
    //prefetch_label[item_id*2+1] = lines_[lines_id_].second;

    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.part_action_with_ctx_data_param().shuffle()) {
        ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(PartActionWithCtxDataLayer);
REGISTER_LAYER_CLASS(PartActionWithCtxData);

}  // namespace caffe
#endif  // USE_OPENCV
