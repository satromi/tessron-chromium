/*
 * tensorflow/lite/model_builder.h for the browser of TessronOS
 *
 *	The audio processing of media/webrtc and WebRTC names a TFLite model
 *	only by reference (the handle of a model of the echo estimator or
 *	the voice isolation); this build carries no TFLite, so the type is
 *	declared and never defined. Found after every other directory
 *	(-idirafter, tools/browser/flags.txt), by the shim's #include_next.
 */
#ifndef BR_TFLITE_MODEL_BUILDER_H
#define BR_TFLITE_MODEL_BUILDER_H

namespace tflite {
class FlatBufferModel;
}

#endif
