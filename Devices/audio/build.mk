# Opt-in speaker and microphone development; requires config/fragments/audio.config.
$(eval $(call module,w202ds-audio-amp,audio/source,w202_audio_amp.ko))
$(eval $(call module,w202ds-audio-dsp,audio/source,w202_audio_dsp.ko,w202ds-modem-mailbox))
$(eval $(call module,w202ds-audio-vbc,audio/source,w202_audio_vbc.ko,w202ds-audio-dsp))
$(eval $(call module,w202ds-audio-pcm,audio/source,w202_audio_pcm.ko))
$(eval $(call module,w202ds-audio-mic,audio/source,w202_audio_mic.ko))
$(eval $(call module,w202ds-audio-card,audio/source,w202_audio_card.ko,w202ds-audio-amp w202ds-audio-vbc w202ds-audio-pcm w202ds-audio-mic))
.PHONY: w202ds-audio-driver
w202ds-audio-driver: w202ds-audio-card
# AW88166/common helpers and sprd-dma are built by the matching kernel config.
# Not in SERVER_MODULES: enabling this target does not deploy or select audio DT.
