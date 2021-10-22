#include "plugin.hpp"


using simd::float_4;


template <typename T>
static T clip(T x) {
	// return std::tanh(x);
	// Pade approximant of tanh
	x = simd::clamp(x, -3.f, 3.f);
	return x * (27 + x * x) / (27 + 9 * x * x);
}


template <typename T>
struct LadderFilter {
	T omega0;
	T resonance = 1;
	T state[4];
	T input;

	LadderFilter() {
		reset();
		setCutoff(0);
	}

	void reset() {
		for (int i = 0; i < 4; i++) {
			state[i] = 0;
		}
	}

	void setCutoff(T cutoff) {
		omega0 = 2 * T(M_PI) * cutoff;
	}

	void process(T input, T dt) {
		dsp::stepRK4(T(0), dt, state, 4, [&](T t, const T x[], T dxdt[]) {
			T inputt = crossfade(this->input, input, t / dt);
			T inputc = clip(inputt - resonance * x[3]);
			T yc0 = clip(x[0]);
			T yc1 = clip(x[1]);
			T yc2 = clip(x[2]);
			T yc3 = clip(x[3]);

			dxdt[0] = omega0 * (inputc - yc0);
			dxdt[1] = omega0 * (yc0 - yc1);
			dxdt[2] = omega0 * (yc1 - yc2);
			dxdt[3] = omega0 * (yc2 - yc3);
		});

		this->input = input;
	}

	T lowpass() {
		return state[3];
	}
	T highpass() {
		// TODO This is incorrect when `resonance > 0`. Is the math wrong?
		return clip((input - resonance * state[3]) - 4 * state[0] + 6 * state[1] - 4 * state[2] + state[3]);
	}
};


static const int UPSAMPLE = 2;

struct VCF : Module {
	enum ParamIds {
		FREQ_PARAM,
		FINE_PARAM, // removed in 2.0
		RES_PARAM,
		FREQ_CV_PARAM,
		DRIVE_PARAM,
		// Added in 2.0
		RES_CV_PARAM,
		DRIVE_CV_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		FREQ_INPUT,
		RES_INPUT,
		DRIVE_INPUT,
		IN_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		LPF_OUTPUT,
		HPF_OUTPUT,
		NUM_OUTPUTS
	};

	LadderFilter<float_4> filters[4];
	// Upsampler<UPSAMPLE, 8> inputUpsampler;
	// Decimator<UPSAMPLE, 8> lowpassDecimator;
	// Decimator<UPSAMPLE, 8> highpassDecimator;

	VCF() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
		// Multiply and offset for backward patch compatibility
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Cutoff frequency", " Hz", std::pow(2, 10.f), dsp::FREQ_C4 / std::pow(2, 5.f));
		configParam(FINE_PARAM, 0.f, 1.f, 0.5f, "Fine frequency");
		configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0.f, 100.f);
		configParam(RES_CV_PARAM, -1.f, 1.f, 0.f, "Resonance CV", "%", 0.f, 100.f);
		configParam(FREQ_CV_PARAM, -1.f, 1.f, 0.f, "Cutoff frequency CV", "%", 0.f, 100.f);
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.f, "Drive", "", 0, 11);
		configParam(DRIVE_CV_PARAM, -1.f, 1.f, 0.f, "Drive CV", "%", 0, 100);
		configInput(FREQ_INPUT, "Frequency");
		configInput(RES_INPUT, "Resonance");
		configInput(DRIVE_INPUT, "Drive");
		configInput(IN_INPUT, "Audio");
		configOutput(LPF_OUTPUT, "Lowpass filter");
		configOutput(HPF_OUTPUT, "Highpass filter");
		configBypass(IN_INPUT, LPF_OUTPUT);
		configBypass(IN_INPUT, HPF_OUTPUT);
	}

	void onReset() override {
		for (int i = 0; i < 4; i++)
			filters[i].reset();
	}

	void process(const ProcessArgs& args) override {
		if (!outputs[LPF_OUTPUT].isConnected() && !outputs[HPF_OUTPUT].isConnected()) {
			return;
		}

		float driveParam = params[DRIVE_PARAM].getValue();
		float driveCvParam = params[DRIVE_CV_PARAM].getValue();
		float resParam = params[RES_PARAM].getValue();
		float resCvParam = params[RES_CV_PARAM].getValue();
		float fineParam = params[FINE_PARAM].getValue();
		fineParam = dsp::quadraticBipolar(fineParam * 2.f - 1.f) * 7.f / 12.f;
		float freqCvParam = params[FREQ_CV_PARAM].getValue();
		freqCvParam = dsp::quadraticBipolar(freqCvParam);
		float freqParam = params[FREQ_PARAM].getValue();
		freqParam = freqParam * 10.f - 5.f;

		int channels = std::max(1, inputs[IN_INPUT].getChannels());

		for (int c = 0; c < channels; c += 4) {
			auto* filter = &filters[c / 4];

			float_4 input = float_4::load(inputs[IN_INPUT].getVoltages(c)) / 5.f;

			// Drive gain
			// TODO Make center of knob unity gain, 0 is off like a VCA
			float_4 drive = driveParam + inputs[DRIVE_INPUT].getPolyVoltageSimd<float_4>(c) / 10.f * driveCvParam;
			drive = clamp(drive, 0.f, 1.f);
			float_4 gain = simd::pow(1.f + drive, 5);
			input *= gain;

			// Add -120dB noise to bootstrap self-oscillation
			input += 1e-6f * (2.f * random::uniform() - 1.f);

			// Set resonance
			float_4 resonance = resParam + inputs[RES_INPUT].getPolyVoltageSimd<float_4>(c) / 10.f * resCvParam;
			resonance = clamp(resonance, 0.f, 1.f);
			filter->resonance = simd::pow(resonance, 2) * 10.f;

			// Get pitch
			float_4 pitch = freqParam + fineParam + inputs[FREQ_INPUT].getPolyVoltageSimd<float_4>(c) * freqCvParam;
			// Set cutoff
			float_4 cutoff = dsp::FREQ_C4 * simd::pow(2.f, pitch);
			cutoff = clamp(cutoff, 1.f, 8000.f);
			filter->setCutoff(cutoff);

			// Set outputs
			filter->process(input, args.sampleTime);
			float_4 lowpass = 5.f * filter->lowpass();
			lowpass.store(outputs[LPF_OUTPUT].getVoltages(c));
			float_4 highpass = 5.f * filter->highpass();
			highpass.store(outputs[HPF_OUTPUT].getVoltages(c));
		}

		outputs[LPF_OUTPUT].setChannels(channels);
		outputs[HPF_OUTPUT].setChannels(channels);

		/*
		// Process sample
		float dt = args.sampleTime / UPSAMPLE;
		float inputBuf[UPSAMPLE];
		float lowpassBuf[UPSAMPLE];
		float highpassBuf[UPSAMPLE];
		inputUpsampler.process(input, inputBuf);
		for (int i = 0; i < UPSAMPLE; i++) {
			// Step the filter
			filter.process(inputBuf[i], dt);
			lowpassBuf[i] = filter.lowpass;
			highpassBuf[i] = filter.highpass;
		}

		// Set outputs
		if (outputs[LPF_OUTPUT].isConnected()) {
			outputs[LPF_OUTPUT].setVoltage(5.f * lowpassDecimator.process(lowpassBuf));
		}
		if (outputs[HPF_OUTPUT].isConnected()) {
			outputs[HPF_OUTPUT].setVoltage(5.f * highpassDecimator.process(highpassBuf));
		}
		*/
	}
};


struct VCFWidget : ModuleWidget {
	VCFWidget(VCF* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VCF.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(17.587, 29.808)), module, VCF::FREQ_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(8.895, 56.388)), module, VCF::RES_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(26.665, 56.388)), module, VCF::DRIVE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(6.996, 80.603)), module, VCF::FREQ_CV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(17.833, 80.603)), module, VCF::RES_CV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(28.67, 80.603)), module, VCF::DRIVE_CV_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.996, 96.813)), module, VCF::FREQ_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.833, 96.813)), module, VCF::RES_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.67, 96.813)), module, VCF::DRIVE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.996, 113.115)), module, VCF::IN_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(17.833, 113.115)), module, VCF::LPF_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(28.67, 113.115)), module, VCF::HPF_OUTPUT));
	}
};


Model* modelVCF = createModel<VCF, VCFWidget>("VCF");
