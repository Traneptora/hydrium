#ifndef HYDRIUM_MODULAR_H_
#define HYDRIUM_MODULAR_H_

#include <stddef.h>
#include <stdint.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"

typedef struct HYDMALeafNode {
    int context;
    int predictor;
    int offset;
    uint32_t multiplier;
} HYDMALeafNode;

typedef struct HYDMADecisionNode {
    int property;
    int32_t value;
    void *left_child;
    void *right_child;
} HYDMADecisionNode;

typedef struct {
    HYDAnsStream *stream;
    size_t num_nodes;
    void *root_node;
} HYDMATree;


#endif /* HYDRIUM_MODULAR_H_ */