# Passthrough with volume/switch Pipeline and PCM
#
# Pipeline Endpoints for connection are :-
#
#  host PCM_C <-- B0 <-- Volume 0 <-- B1 <-- source DAI0

# Include topology builder
include(`utils.m4')
include(`buffer.m4')
include(`pcm.m4')
include(`pga.m4')
include(`dai.m4')
include(`mixercontrol.m4')
include(`pipeline.m4')

#
# Controls
#
# Volume Mixer control with max value of 32
C_CONTROLMIXER(Master Capture Volume, PIPELINE_ID,
	CONTROLMIXER_OPS(volsw, 256 binds the mixer control to volume get/put handlers, 256, 256),
	CONTROLMIXER_MAX(, 80),
	false,
	CONTROLMIXER_TLV(TLV 80 steps from -50dB to +30dB for 1dB, vtlv_m50s1),
	Channel register and shift for Front Left/Right,
	LIST(`	', KCONTROL_CHANNEL(FL, 1, 0), KCONTROL_CHANNEL(FR, 1, 1)))

C_CONTROLMIXER(Master Capture Switch, PIPELINE_ID,
	CONTROLMIXER_OPS(volsw, 259 binds the mixer control to switch get/put handlers, 259, 259),
	CONTROLMIXER_MAX(max 1 indicates switch type control, 1),
	false,
	,
	Channel register and shift for Front Left/Right,
	LIST(`	', KCONTROL_CHANNEL(FL, 2, 0), KCONTROL_CHANNEL(FR, 2, 1)),
	"1", "1")

#
# Volume Configuration
#

W_VENDORTUPLES(capture_pga_tokens, sof_volume_tokens,
LIST(`		', `SOF_TKN_VOLUME_RAMP_STEP_TYPE	"0"'
     `		', `SOF_TKN_VOLUME_RAMP_STEP_MS		"250"'))

W_DATA(capture_pga_conf, capture_pga_tokens)

#
# Components and Buffers
#

# Host "Passthrough Capture" PCM
# with 0 sink and 2 source periods
W_PCM_CAPTURE(PCM_ID, Passthrough Capture, 0, 2, SCHEDULE_CORE)

# "Volume" has x source and 2 sink periods
W_PGA(0, PIPELINE_FORMAT, 2, DAI_PERIODS, capture_pga_conf, SCHEDULE_CORE,
	LIST(`		', "PIPELINE_ID Master Capture Volume", "PIPELINE_ID Master Capture Switch"))

# Capture Buffers
W_BUFFER(0, COMP_BUFFER_SIZE(2,
	COMP_SAMPLE_SIZE(PIPELINE_FORMAT), PIPELINE_CHANNELS, COMP_PERIOD_FRAMES(PCM_MAX_RATE, SCHEDULE_PERIOD)),
	PLATFORM_HOST_MEM_CAP)
W_BUFFER(1, COMP_BUFFER_SIZE(DAI_PERIODS,
	COMP_SAMPLE_SIZE(DAI_FORMAT), PIPELINE_CHANNELS, COMP_PERIOD_FRAMES(PCM_MAX_RATE, SCHEDULE_PERIOD)),
	PLATFORM_DAI_MEM_CAP)

#
# Pipeline Graph
#
#  host PCM_P <-- B0 <-- Volume 0 <-- B1 <-- sink DAI0

P_GRAPH(pipe-pass-vol-capture-PIPELINE_ID, PIPELINE_ID,
	LIST(`		',
	`dapm(N_PCMC(PCM_ID), N_BUFFER(0))',
	`dapm(N_BUFFER(0), N_PGA(0))',
	`dapm(N_PGA(0), N_BUFFER(1))'))

#
# Pipeline Source and Sinks
#
indir(`define', concat(`PIPELINE_SINK_', PIPELINE_ID), N_BUFFER(1))
indir(`define', concat(`PIPELINE_PCM_', PIPELINE_ID), Passthrough Capture PCM_ID)

#
# PCM Configuration
#

PCM_CAPABILITIES(Passthrough Capture PCM_ID, `S32_LE,S24_LE,S16_LE',
	PCM_MIN_RATE, PCM_MAX_RATE, PIPELINE_CHANNELS, PIPELINE_CHANNELS,
	2, 16, 192, 16384, 65536, 65536)
