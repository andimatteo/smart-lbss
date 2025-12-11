#ifndef EML_NET_H
#define EML_NET_H

#include <stdint.h>

typedef enum {
    EmlNetActivationRelu,
    EmlNetActivationIdentity
} EmlNetActivationFunction;

typedef struct {
    int n_outputs;
    int n_inputs;
    const float *weights;
    const float *biases;
    EmlNetActivationFunction activation;
} EmlNetLayer;

typedef struct {
    int n_layers;
    const EmlNetLayer *layers;
    float *buf1;
    float *buf2;
    int max_layer_size;
} EmlNet;

// Prototipi per l'interfaccia usata dai file .h generati
int32_t eml_net_predict(const EmlNet *model, const float *features, int32_t n_features);
int32_t eml_net_regress(const EmlNet *model, const float *features, int32_t n_features, float *out, int32_t out_length);
float eml_net_regress1(const EmlNet *model, const float *features, int32_t n_features);

#endif
