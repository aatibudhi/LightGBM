#include "serial_tree_learner.h"

#include <LightGBM/utils/array_args.h>

#include <algorithm>
#include <vector>

namespace LightGBM {

SerialTreeLearner::SerialTreeLearner(const TreeConfig& tree_config)
  :data_partition_(nullptr), is_feature_used_(nullptr), smaller_leaf_histogram_array_(nullptr),
  larger_leaf_histogram_array_(nullptr),
  smaller_leaf_splits_(nullptr), larger_leaf_splits_(nullptr),
  ordered_gradients_(nullptr), ordered_hessians_(nullptr), is_data_in_leaf_(nullptr) {
  // initialize with nullptr
  num_leaves_ = tree_config.num_leaves;
  min_num_data_one_leaf_ = static_cast<data_size_t>(tree_config.min_data_in_leaf);
  min_sum_hessian_one_leaf_ = static_cast<double>(tree_config.min_sum_hessian_in_leaf);
  feature_fraction_ = tree_config.feature_fraction;
  random_ = Random(tree_config.feature_fraction_seed);
  histogram_pool_size_ = tree_config.histogram_pool_size;
  max_depth_ = tree_config.max_depth;
}

SerialTreeLearner::~SerialTreeLearner() {
  if (data_partition_ != nullptr) { delete data_partition_; }
  if (smaller_leaf_splits_ != nullptr) { delete smaller_leaf_splits_; }
  if (larger_leaf_splits_ != nullptr) { delete larger_leaf_splits_; }
  for (int i = 0; i < num_leaves_; ++i) {
    FeatureHistogram* ptr = nullptr;
    if (histogram_pool_.Get(i, &ptr)) {
      delete[] ptr;
    }
  }
  if (is_feature_used_ != nullptr) { delete[] is_feature_used_; }
  if (ordered_gradients_ != nullptr) { delete[] ordered_gradients_; }
  if (ordered_hessians_ != nullptr) { delete[] ordered_hessians_; }
  for (auto& bin : ordered_bins_) {
    delete bin;
  }
  if (is_data_in_leaf_ != nullptr) {
    delete[] is_data_in_leaf_;
  }
}

void SerialTreeLearner::Init(const Dataset* train_data) {
  train_data_ = train_data;
  num_data_ = train_data_->num_data();
  num_features_ = train_data_->num_features();
  int max_cache_size = 0;
  // Get the max size of pool
  if (histogram_pool_size_ < 0) {
    max_cache_size = num_leaves_;
  } else {
    size_t total_histogram_size = 0;
    for (int i = 0; i < train_data_->num_features(); ++i) {
      total_histogram_size += sizeof(HistogramBinEntry) * train_data_->FeatureAt(i)->num_bin();
    }
    max_cache_size = static_cast<int>(histogram_pool_size_ * 1024 * 1024 / total_histogram_size);
  }
  // at least need 2 leaves
  max_cache_size = Common::Max(2, max_cache_size);
  max_cache_size = Common::Min(max_cache_size, num_leaves_);
  histogram_pool_.ResetSize(max_cache_size, num_leaves_);

  auto histogram_create_function = [this]() {
    FeatureHistogram* tmp_histogram_array = new FeatureHistogram[train_data_->num_features()];
    for (int j = 0; j < train_data_->num_features(); ++j) {
      tmp_histogram_array[j].Init(train_data_->FeatureAt(j),
        j, min_num_data_one_leaf_,
        min_sum_hessian_one_leaf_);
    }
    return tmp_histogram_array;
  };
  histogram_pool_.Fill(histogram_create_function);

  // push split information for all leaves
  for (int i = 0; i < num_leaves_; ++i) {
    best_split_per_leaf_.push_back(SplitInfo());
  }
  // initialize ordered_bins_ with nullptr
  for (int i = 0; i < num_features_; ++i) {
    ordered_bins_.push_back(nullptr);
  }

  // get ordered bin
  #pragma omp parallel for schedule(guided)
  for (int i = 0; i < num_features_; ++i) {
    ordered_bins_[i] = train_data_->FeatureAt(i)->bin_data()->CreateOrderedBin();
  }

  // check existing for ordered bin
  for (int i = 0; i < num_features_; ++i) {
    if (ordered_bins_[i] != nullptr) {
      has_ordered_bin_ = true;
      break;
    }
  }
  // initialize splits for leaf
  smaller_leaf_splits_ = new LeafSplits(train_data_->num_features(), train_data_->num_data());
  larger_leaf_splits_ = new LeafSplits(train_data_->num_features(), train_data_->num_data());

  // initialize data partition
  data_partition_ = new DataPartition(num_data_, num_leaves_);

  is_feature_used_ = new bool[num_features_];

  // initialize ordered gradients and hessians
  ordered_gradients_ = new score_t[num_data_];
  ordered_hessians_ = new score_t[num_data_];
  // if has ordered bin, need allocate a buffer to fast split
  if (has_ordered_bin_) {
    is_data_in_leaf_ = new char[num_data_];
  }
  Log::Info("Number of data:%d, Number of features:%d", num_data_, num_features_);
}


Tree* SerialTreeLearner::Train(const score_t* gradients, const score_t *hessians) {
  gradients_ = gradients;
  hessians_ = hessians;
  // some initial works before training
  BeforeTrain();
  Tree *tree = new Tree(num_leaves_);
  // save pointer to last trained tree
  last_trained_tree_ = tree;
  // root leaf
  int left_leaf = 0;
  // only root leaf can be splitted on first time
  int right_leaf = -1;
  for (int split = 0; split < num_leaves_ - 1; split++) {
    // some initial works before finding best split
    if (BeforeFindBestSplit(left_leaf, right_leaf)) {
      // find best threshold for every feature
      FindBestThresholds();
      // find best split from all features
      FindBestSplitsForLeaves();
    }
    // Get a leaf with max split gain
    int best_leaf = static_cast<int>(ArrayArgs<SplitInfo>::ArgMax(best_split_per_leaf_));
    // Get split information for best leaf
    const SplitInfo& best_leaf_SplitInfo = best_split_per_leaf_[best_leaf];
    // cannot split, quit
    if (best_leaf_SplitInfo.gain <= 0.0) {
      Log::Info("cannot find more split with gain = %f , current #leaves=%d",
                   best_leaf_SplitInfo.gain, split + 1);
      break;
    }
    // split tree with best leaf
    Split(tree, best_leaf, &left_leaf, &right_leaf);
  }
  return tree;
}

void SerialTreeLearner::BeforeTrain() {
  // reset histogram pool
  histogram_pool_.ResetMap();
  // initialize used features
  for (int i = 0; i < num_features_; ++i) {
    is_feature_used_[i] = false;
  }
  // Get used feature at current tree
  size_t used_feature_cnt = static_cast<size_t>(num_features_*feature_fraction_);
  std::vector<size_t> used_feature_indices = random_.Sample(num_features_, used_feature_cnt);
  for (auto idx : used_feature_indices) {
    is_feature_used_[idx] = true;
  }

  // initialize data partition
  data_partition_->Init();

  // reset the splits for leaves
  for (int i = 0; i < num_leaves_; ++i) {
    best_split_per_leaf_[i].Reset();
  }

  // Sumup for root
  if (data_partition_->leaf_count(0) == num_data_) {
    // use all data
    smaller_leaf_splits_->Init(gradients_, hessians_);
    // point to gradients, avoid copy
    ptr_to_ordered_gradients_smaller_leaf_ = gradients_;
    ptr_to_ordered_hessians_smaller_leaf_  = hessians_;
  } else {
    // use bagging, only use part of data
    smaller_leaf_splits_->Init(0, data_partition_, gradients_, hessians_);
    // copy used gradients and hessians to ordered buffer
    const data_size_t* indices = data_partition_->indices();
    data_size_t cnt = data_partition_->leaf_count(0);
    #pragma omp parallel for schedule(static)
    for (data_size_t i = 0; i < cnt; ++i) {
      ordered_gradients_[i] = gradients_[indices[i]];
      ordered_hessians_[i] = hessians_[indices[i]];
    }
    // point to ordered_gradients_ and ordered_hessians_
    ptr_to_ordered_gradients_smaller_leaf_ = ordered_gradients_;
    ptr_to_ordered_hessians_smaller_leaf_ = ordered_hessians_;
  }

  ptr_to_ordered_gradients_larger_leaf_ = nullptr;
  ptr_to_ordered_hessians_larger_leaf_ = nullptr;

  larger_leaf_splits_->Init();

  // if has ordered bin, need to initialize the ordered bin
  if (has_ordered_bin_) {
    if (data_partition_->leaf_count(0) == num_data_) {
      // use all data, pass nullptr
      #pragma omp parallel for schedule(guided)
      for (int i = 0; i < num_features_; ++i) {
        if (ordered_bins_[i] != nullptr) {
          ordered_bins_[i]->Init(nullptr, num_leaves_);
        }
      }
    } else {
      // bagging, only use part of data

      // mark used data
      std::memset(is_data_in_leaf_, 0, sizeof(char)*num_data_);
      const data_size_t* indices = data_partition_->indices();
      data_size_t begin = data_partition_->leaf_begin(0);
      data_size_t end = begin + data_partition_->leaf_count(0);
      #pragma omp parallel for schedule(static)
      for (data_size_t i = begin; i < end; ++i) {
        is_data_in_leaf_[indices[i]] = 1;
      }
      // initialize ordered bin
      #pragma omp parallel for schedule(guided)
      for (int i = 0; i < num_features_; ++i) {
        if (ordered_bins_[i] != nullptr) {
          ordered_bins_[i]->Init(is_data_in_leaf_, num_leaves_);
        }
      }
    }
  }
}

bool SerialTreeLearner::BeforeFindBestSplit(int left_leaf, int right_leaf) {
  // check depth of current leaf
  if (max_depth_ > 0) {
    // only need to check left leaf, since right leaf is in same level of left leaf
    if (last_trained_tree_->leaf_depth(left_leaf) >= max_depth_) {
      best_split_per_leaf_[left_leaf].gain = kMinScore;
      if (right_leaf >= 0) {
        best_split_per_leaf_[right_leaf].gain = kMinScore;
      }
      return false;
    }
  }
  data_size_t num_data_in_left_child = GetGlobalDataCountInLeaf(left_leaf);
  data_size_t num_data_in_right_child = GetGlobalDataCountInLeaf(right_leaf);
  // no enough data to continue
  if (num_data_in_right_child < static_cast<data_size_t>(min_num_data_one_leaf_ * 2)
    && num_data_in_left_child < static_cast<data_size_t>(min_num_data_one_leaf_ * 2)) {
    best_split_per_leaf_[left_leaf].gain = kMinScore;
    if (right_leaf >= 0) {
      best_split_per_leaf_[right_leaf].gain = kMinScore;
    }
    return false;
  }
  parent_leaf_histogram_array_ = nullptr;
  // -1 if only has one leaf. else equal the index of smaller leaf
  int smaller_leaf = -1;
  int larger_leaf = -1;
  // only have root
  if (right_leaf < 0) {
    histogram_pool_.Get(left_leaf, &smaller_leaf_histogram_array_);
    larger_leaf_histogram_array_ = nullptr;
    
  } else if (num_data_in_left_child < num_data_in_right_child) {
    smaller_leaf = left_leaf;
    larger_leaf = right_leaf;
    // put parent(left) leaf's histograms into larger leaf's histograms
    if (histogram_pool_.Get(left_leaf, &larger_leaf_histogram_array_)) { parent_leaf_histogram_array_ = larger_leaf_histogram_array_; }
    histogram_pool_.Move(left_leaf, right_leaf);
    histogram_pool_.Get(left_leaf, &smaller_leaf_histogram_array_);
  } else {
    smaller_leaf = right_leaf;
    larger_leaf = left_leaf;
    // put parent(left) leaf's histograms to larger leaf's histograms
    if (histogram_pool_.Get(left_leaf, &larger_leaf_histogram_array_)) { parent_leaf_histogram_array_ = larger_leaf_histogram_array_; }
    histogram_pool_.Get(right_leaf, &smaller_leaf_histogram_array_);
  }

  // init for the ordered gradients, only initialize when have 2 leaves
  if (smaller_leaf >= 0) {
    // only need to initialize for smaller leaf

    // Get leaf boundary
    const data_size_t* indices = data_partition_->indices();
    data_size_t begin = data_partition_->leaf_begin(smaller_leaf);
    data_size_t end = begin + data_partition_->leaf_count(smaller_leaf);
    // copy
    #pragma omp parallel for schedule(static)
    for (data_size_t i = begin; i < end; ++i) {
      ordered_gradients_[i - begin] = gradients_[indices[i]];
      ordered_hessians_[i - begin] = hessians_[indices[i]];
    }
    // assign pointer
    ptr_to_ordered_gradients_smaller_leaf_ = ordered_gradients_;
    ptr_to_ordered_hessians_smaller_leaf_ = ordered_hessians_;

    if (parent_leaf_histogram_array_ == nullptr) {
      // need order gradient for larger leaf
      data_size_t smaller_size = end - begin;
      data_size_t larger_begin = data_partition_->leaf_begin(larger_leaf);
      data_size_t larger_end = larger_begin + data_partition_->leaf_count(larger_leaf);
      // copy
      #pragma omp parallel for schedule(static)
      for (data_size_t i = larger_begin; i < larger_end; ++i) {
        ordered_gradients_[smaller_size + i - larger_begin] = gradients_[indices[i]];
        ordered_hessians_[smaller_size + i - larger_begin] = hessians_[indices[i]];
      }
      ptr_to_ordered_gradients_larger_leaf_ = ordered_gradients_ + smaller_size;
      ptr_to_ordered_hessians_larger_leaf_ = ordered_hessians_ + smaller_size;
    }
  }

  // split for the ordered bin
  if (has_ordered_bin_ && right_leaf >= 0) {
    // mark data that at left-leaf
    std::memset(is_data_in_leaf_, 0, sizeof(char)*num_data_);
    const data_size_t* indices = data_partition_->indices();
    data_size_t begin = data_partition_->leaf_begin(left_leaf);
    data_size_t end = begin + data_partition_->leaf_count(left_leaf);
    #pragma omp parallel for schedule(static)
    for (data_size_t i = begin; i < end; ++i) {
      is_data_in_leaf_[indices[i]] = 1;
    }
    // split the ordered bin
    #pragma omp parallel for schedule(guided)
    for (int i = 0; i < num_features_; ++i) {
      if (ordered_bins_[i] != nullptr) {
        ordered_bins_[i]->Split(left_leaf, right_leaf, is_data_in_leaf_);
      }
    }
  }
  return true;
}


void SerialTreeLearner::FindBestThresholds() {
  #pragma omp parallel for schedule(guided)
  for (int feature_index = 0; feature_index < num_features_; feature_index++) {
    // feature is not used
    if ((is_feature_used_ != nullptr && is_feature_used_[feature_index] == false)) continue;
    // if parent(larger) leaf cannot split at current feature
    if (parent_leaf_histogram_array_ != nullptr && !parent_leaf_histogram_array_[feature_index].is_splittable()) {
      smaller_leaf_histogram_array_[feature_index].set_is_splittable(false);
      continue;
    }

    // construct histograms for smaller leaf
    if (ordered_bins_[feature_index] == nullptr) {
      // if not use ordered bin
      smaller_leaf_histogram_array_[feature_index].Construct(smaller_leaf_splits_->data_indices(),
        smaller_leaf_splits_->num_data_in_leaf(),
        smaller_leaf_splits_->sum_gradients(),
        smaller_leaf_splits_->sum_hessians(),
        ptr_to_ordered_gradients_smaller_leaf_,
        ptr_to_ordered_hessians_smaller_leaf_);
    } else {
      // used ordered bin
      smaller_leaf_histogram_array_[feature_index].Construct(ordered_bins_[feature_index],
        smaller_leaf_splits_->LeafIndex(),
        smaller_leaf_splits_->num_data_in_leaf(),
        smaller_leaf_splits_->sum_gradients(),
        smaller_leaf_splits_->sum_hessians(),
        gradients_,
        hessians_);
    }
    // find best threshold for smaller child
    smaller_leaf_histogram_array_[feature_index].FindBestThreshold(&smaller_leaf_splits_->BestSplitPerFeature()[feature_index]);

    // only has root leaf
    if (larger_leaf_splits_ == nullptr || larger_leaf_splits_->LeafIndex() < 0) continue;

    if (parent_leaf_histogram_array_ != nullptr) {
      // construct histgroms for large leaf, we initialize larger leaf as the parent,
      // so we can just subtract the smaller leaf's histograms
      larger_leaf_histogram_array_[feature_index].Subtract(smaller_leaf_histogram_array_[feature_index]);
    } else {
      if (ordered_bins_[feature_index] == nullptr) {
        // if not use ordered bin
        larger_leaf_histogram_array_[feature_index].Construct(larger_leaf_splits_->data_indices(),
          larger_leaf_splits_->num_data_in_leaf(),
          larger_leaf_splits_->sum_gradients(),
          larger_leaf_splits_->sum_hessians(),
          ptr_to_ordered_gradients_larger_leaf_,
          ptr_to_ordered_hessians_larger_leaf_);
      } else {
        // used ordered bin
        larger_leaf_histogram_array_[feature_index].Construct(ordered_bins_[feature_index],
          larger_leaf_splits_->LeafIndex(),
          larger_leaf_splits_->num_data_in_leaf(),
          larger_leaf_splits_->sum_gradients(),
          larger_leaf_splits_->sum_hessians(),
          gradients_,
          hessians_);
      }
    }

    // find best threshold for larger child
    larger_leaf_histogram_array_[feature_index].FindBestThreshold(&larger_leaf_splits_->BestSplitPerFeature()[feature_index]);
  }
}


void SerialTreeLearner::Split(Tree* tree, int best_Leaf, int* left_leaf, int* right_leaf) {
  const SplitInfo& best_split_info = best_split_per_leaf_[best_Leaf];

  // left = parent
  *left_leaf = best_Leaf;
  // split tree, will return right leaf
  *right_leaf = tree->Split(best_Leaf, best_split_info.feature, best_split_info.threshold,
    train_data_->FeatureAt(best_split_info.feature)->feature_index(),
    train_data_->FeatureAt(best_split_info.feature)->BinToValue(best_split_info.threshold),
    static_cast<double>(best_split_info.left_output),
    static_cast<double>(best_split_info.right_output),
    static_cast<double>(best_split_info.gain));

  // split data partition
  data_partition_->Split(best_Leaf, train_data_->FeatureAt(best_split_info.feature)->bin_data(),
                         best_split_info.threshold, *right_leaf);

  // init the leaves that used on next iteration
  if (best_split_info.left_count < best_split_info.right_count) {
    smaller_leaf_splits_->Init(*left_leaf, data_partition_,
                               best_split_info.left_sum_gradient,
                               best_split_info.left_sum_hessian);
    larger_leaf_splits_->Init(*right_leaf, data_partition_,
                               best_split_info.right_sum_gradient,
                               best_split_info.right_sum_hessian);
  } else {
    smaller_leaf_splits_->Init(*right_leaf, data_partition_, best_split_info.right_sum_gradient, best_split_info.right_sum_hessian);
    larger_leaf_splits_->Init(*left_leaf, data_partition_, best_split_info.left_sum_gradient, best_split_info.left_sum_hessian);
  }
}

}  // namespace LightGBM
