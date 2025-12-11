#include "eml_net.h"
#include <string.h>
#include <math.h>

static float activate(float x, EmlNetActivationFunction act) {
    if (act == EmlNetActivationRelu) {
        return x > 0.0f ? x : 0.0f;
    }
    return x;
}

int32_t eml_net_regress(const EmlNet *model, const float *features, int32_t n_features, float *out, int32_t out_length) {
    const float *input = features;
    float *output = model->buf1;
    
    for (int l = 0; l < model->n_layers; l++) {
        const EmlNetLayer *layer = &model->layers[l];
        output = (l % 2 == 0) ? model->buf1 : model->buf2;
        const float *prev_input = (l == 0) ? input : ((l % 2 == 0) ? model->buf2 : model->buf1);

        for (int i = 0; i < layer->n_outputs; i++) {
            float sum = layer->biases[i];
            for (int j = 0; j < layer->n_inputs; j++) {
                sum += prev_input[j] * layer->weights[i * layer->n_inputs + j];
            }
            output[i] = activate(sum, layer->activation);
        }
    }
    for(int i=0; i<out_length; i++) out[i] = output[i];
    return 0;
}

float eml_net_regress1(const EmlNet *model, const float *features, int32_t n_features) {
    float val;
    eml_net_regress(model, features, n_features, &val, 1);
    return val;
}

int32_t eml_net_predict(const EmlNet *model, const float *features, int32_t n_features) { return 0; }
