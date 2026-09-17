---
name: mic-adc-front-end
description: Confirmed on hardware - PA6 is ADC1_INP3, ADC EXTSEL 13 is TIM6_TRGO, the preamp biases near mid scale, and ADC3 has to be configured on a part that has none.
metadata:
  node_type: memory
  type: project
---

The microphone front end, measured on the bench 2026-09-16 with the driver in
`src/pika/audio/ADCMicrophone.cpp`. None of the first three facts is derivable
from anything in this tree - ChibiOS carries no pin-to-channel map and no H7
EXTSEL table, and its only H7 ADC example has the trigger line commented out.

- **PA6 is ADC1_INP3**, so `pcsel` is `ADC_SELMASK_IN3` and the sequence entry
  is `ADC_SQR1_SQ1_N(ADC_CHANNEL_IN3)`. Confirmed from the raw samples: with
  the preamp powered the input sits at **2041-2042 codes** against a mid scale
  of 2048, which is why the driver simply subtracts mid scale and no longer
  tracks the bias. A wrong channel parks it at an end of the range instead.
- **EXTSEL 13 is TIM6_TRGO for ADC1/2**, from RM0468. Confirmed by rate: a 2s
  capture produced exactly 250 blocks of 256 samples, which is 32000 samples a
  second to the block. This is *not* the number the DAC uses for the same
  timer - DAC TSEL 5 and ADC EXTSEL 13 are the same TRGO through different
  trigger tables.
- **PA7 (`LINE_MIC_SHDN`) is active high shutdown**: drive it *low* to make the
  microphone run. It is the reverse of `LINE_SPK_EN` two lines above it in
  board.h, which is a genuine active high enable. It used to be called
  `LINE_MIC_EN`, which said the opposite of what the pin does.
- A quiet room reads **rms around 20-40 and peak around 50-120** in signed 16
  bit full scale, i.e. one to four ADC codes of noise. Nonzero and *varying* is
  the thing to check: a DMA that moved nothing would give a constant, which
  reads as a fixed rms and a peak hold that never moves.

**`STM32_ADC_ADC3_CLOCK_MODE` must be set in mcuconf.h even though this part
has no ADC3.** `STM32_HAS_ADC3` is FALSE and `STM32_ADC_USE_ADC3` defaults
FALSE, but ADCv4's header computes `STM32_ADC3_CLOCK` and range checks it
unconditionally; its default of AHB/4 is 65MHz here, over the 50MHz maximum,
and the build dies on `#error "STM32_ADC3_CLOCK exceeding maximum frequency"`
with nothing to say that the converter does not exist. Point it at
`ADC_CCR_CKMODE_ADCCK` like ADC12.

Two ADCv4 details that fail silently rather than loudly:

- `pcsel` is copied verbatim and never derived from the sequence
  (`hal_adc_lld.c:828`). Omit the bit and the converter runs perfectly and
  reads nothing.
- Do **not** put `ADC_SQR1_NUM_CH` in `sqr[0]`. The driver ORs it in from
  `num_channels` (`hal_adc_lld.c:839`) and setting both corrupts the length.

The sample buffer (`ADCMicrophone::mic_buffer`) goes in `.nocache`, for the
reason in [[i2c-dma-silently-broken]] - ADCv4 does no cache maintenance of its
own, so the section is the whole of the answer.

The speaker and the microphone share one timer's TRGO, because the device
either talks or listens and never both. `pika::audio::SampleClock` owns it and
makes that exclusive: whichever converter claims it holds it until it stops,
and the other is refused. Verified live - `mic.start_capture()` called from gdb
during playback returns false and leaves the stream running.
