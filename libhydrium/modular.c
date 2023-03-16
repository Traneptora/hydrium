#include "entropy.h"
#include "internal.h"
#include "modular.h"

int hyd_init_singleton_ma_tree(HYDEncoder *encoder, HYDMATree *tree, const HYDMALeafNode *leaf_node) {
    HYDEntropyStream stream;
    hyd_init_ans_stream(&stream, &encoder->working_writer);
    hyd_write_cluster_map(&stream, (const uint8_t[1]){0}, 1);

}
