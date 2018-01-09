import unittest
import numpy as np
import sys
import math
from op_test import OpTest


class TestPriorBoxOp(OpTest):
    def set_data(self):
        self.init_test_params()
        self.init_test_input()
        self.init_test_output()
        self.inputs = {'Input': self.input, 'Image': self.image}

        self.attrs = {
            'min_sizes': self.min_sizes,
            'max_sizes': self.max_sizes,
            'aspect_ratios': self.aspect_ratios,
            'variances': self.variances,
            'flip': self.flip,
            'clip': self.clip,
            'step_w': self.step_w,
            'step_h': self.step_h,
            'img_w': self.image_w,
            'img_h': self.image_h,
            'offset': self.offset
        }

        self.outputs = {'Out': self.output}

    def test_check_output(self):
        self.check_output()

    def test_check_grad(self):
        return

    def setUp(self):
        self.op_type = "prior_box"
        self.set_data()

    def init_test_params(self):
        self.layer_w = 4
        self.layer_h = 4

        self.image_w = 20
        self.image_h = 20

        self.step_w = float(self.image_w) / float(self.layer_w)
        self.step_h = float(self.image_h) / float(self.layer_h)

        self.input_channels = 2
        self.image_channels = 3
        self.batch_size = 10

        self.min_sizes = [2, 4]
        self.min_sizes = np.array(self.min_sizes).astype('int64')
        self.max_sizes = [5, 10]
        self.max_sizes = np.array(self.max_sizes).astype('int64')
        self.aspect_ratios = [2.0, 3.0]
        self.flip = True
        self.real_aspect_ratios = [1, 2.0, 1.0 / 2.0, 3.0, 1.0 / 3.0]
        self.aspect_ratios = np.array(
            self.aspect_ratios, dtype=np.float).flatten()
        self.variances = [0.1, 0.1, 0.2, 0.2]
        self.variances = np.array(self.variances, dtype=np.float).flatten()

        self.clip = True

        self.num_priors = len(self.real_aspect_ratios) * len(self.min_sizes)
        if len(self.max_sizes) > 1:
            self.num_priors += len(self.max_sizes)
        self.offset = 0.5

    def init_test_input(self):
        self.image = np.random.random(
            (self.batch_size, self.image_channels, self.image_w,
             self.image_h)).astype('float32')

        self.input = np.random.random(
            (self.batch_size, self.input_channels, self.layer_w,
             self.layer_h)).astype('float32')

    def init_test_output(self):
        out_dim = (2, self.layer_h, self.layer_w, self.num_priors, 4)
        output = np.zeros(out_dim).astype('float32')

        idx = 0
        for h in range(self.layer_h):
            for w in range(self.layer_w):
                center_x = (w + self.offset) * self.step_w
                center_y = (h + self.offset) * self.step_h
                idx = 0
                for s in range(len(self.min_sizes)):
                    min_size = self.min_sizes[s]
                    # first prior: aspect_ratio = 1, size = min_size
                    box_width = box_height = min_size
                    # xmin
                    output[0, h, w, idx, 0] = (
                        center_x - box_width / 2.) / self.image_w
                    # ymin
                    output[0, h, w, idx, 1] = (
                        center_y - box_height / 2.) / self.image_h
                    # xmax
                    output[0, h, w, idx, 2] = (
                        center_x + box_width / 2.) / self.image_w
                    # ymax
                    output[0, h, w, idx, 3] = (
                        center_y + box_height / 2.) / self.image_h
                    idx += 1

                    if len(self.max_sizes) > 0:
                        max_size = self.max_sizes[s]
                        # second prior: aspect_ratio = 1,
                        # size = sqrt(min_size * max_size)
                        box_width = box_height = math.sqrt(min_size * max_size)
                        # xmin
                        output[0, h, w, idx, 0] = (
                            center_x - box_width / 2.) / self.image_w
                        # ymin
                        output[0, h, w, idx, 1] = (
                            center_y - box_height / 2.) / self.image_h
                        # xmax
                        output[0, h, w, idx, 2] = (
                            center_x + box_width / 2.) / self.image_w
                        # ymax
                        output[0, h, w, idx, 3] = (
                            center_y + box_height / 2.) / self.image_h
                        idx += 1

                    # rest of priors
                    for r in range(len(self.real_aspect_ratios)):
                        ar = self.real_aspect_ratios[r]
                        if math.fabs(ar - 1.) < 1e-6:
                            continue
                        box_width = min_size * math.sqrt(ar)
                        box_height = min_size / math.sqrt(ar)
                        # xmin
                        output[0, h, w, idx, 0] = (
                            center_x - box_width / 2.) / self.image_w
                        # ymin
                        output[0, h, w, idx, 1] = (
                            center_y - box_height / 2.) / self.image_h
                        # xmax
                        output[0, h, w, idx, 2] = (
                            center_x + box_width / 2.) / self.image_w
                        # ymax
                        output[0, h, w, idx, 3] = (
                            center_y + box_height / 2.) / self.image_h
                        idx += 1
        # clip the prior's coordidate such that it is within[0, 1]
        if self.clip:
            for h in range(self.layer_h):
                for w in range(self.layer_w):
                    for i in range(self.num_priors):
                        for j in range(4):
                            output[0, h, w, i, j] = min(
                                max(output[0, h, w, i, j], 0), 1)
        # set the variance.
        for h in range(self.layer_h):
            for w in range(self.layer_w):
                for i in range(self.num_priors):
                    for j in range(4):
                        if len(self.variances) == 1:
                            output[1, h, w, i, j] = self.variances[0]
                        else:
                            output[1, h, w, i, j] = self.variances[j]
        self.output = output.astype('float32')


if __name__ == '__main__':
    unittest.main()
