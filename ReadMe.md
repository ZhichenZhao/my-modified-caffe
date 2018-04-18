# My modified caffe

This is a modified version of Caffe, I add some features in this version:

* layers to support part action network (see my repo of "Part Action Network"), e.g. part_action_with_ctx.cpp/hpp
* add scale jittering as data augmentation method. i.e. data_transformer.cpp
if you want use the official version, please see https://github.com/BVLC/caffe
