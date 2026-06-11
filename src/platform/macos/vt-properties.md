h.264 VideoToolbox properties
=============================
Generated from `VTSessionCopySupportedPropertyDictionary(h264_session)`.

Here's the full set organized by function. Notation: **B** = Boolean, **N** = Number, **E** = Enumeration, **RW**/**RO** = read-write status; properties with no annotation exposed no type/status metadata.

## Rate Control & Bitrate
- `AverageBitRate` — N, RW
- `ConstantBitRate` — N, RW
- `VariableBitRate` — N, RW
- `DataRateLimits` — RW
- `ConvergenceDurationForAverageDataRate` — N, RW
- `FrameRateTargetForAverageBitrate` — N, RW
- `VBVBufferDuration` — N, RW
- `VBVInitialDelayPercentage` — N, RW
- `VBVMaxBitRate` — N, RW

## Quality & Quantization
- `Quality` — N, RW
- `ConstantQualityFactor` — N, RW
- `MinAllowedFrameQP` / `MaxAllowedFrameQP` — N, RW
- `SoftMinQuantizationParameter` / `SoftMaxQuantizationParameter` — N, RW
- `SpatialAdaptiveQPLevel` — N, RW
- `QuantizationScalingMatrixPreset` — N, RW
- `ChromaQPIndexOffsetMultiPPS` — N, RW
- `PerceptualQualityOptimization` — B, RW
- `PrioritizeEncodingSpeedOverQuality` — B, RW

## GOP Structure & Reference Frames
- `AllowTemporalCompression` — B, RW
- `AllowFrameReordering` — B, RW *(i.e. B-frames)*
- `AllowOpenGOP` — B, RW
- `MaxKeyFrameInterval` / `MaxKeyFrameIntervalDuration` — N, RW
- `StrictKeyFrameInterval` / `StrictKeyFrameIntervalDuration` — N, RW
- `EnableMultiReferenceP` — B, RW
- `MultiReferencePSpacing` — N, RW
- `UseLongTermReference` — B, RW
- `LookAheadFrames` — N, RW
- `NumberOfSlices` — N, RW
- `DPBRequirements` — RW
- `MaxFrameDelayCount` — N, RO

## Frame Rate & Timing
- `RealTime` — B, RW
- `ExpectedFrameRate` — N, RW
- `ExpectedDuration` — N, RW
- `AverageNonDroppableFrameRate` — N, RW
- `MaximumRealTimeFrameRate` — N, RW
- `SourceFrameCount` — N, RW
- `MoreFramesBeforeStart` / `MoreFramesAfterEnd` — B, RW

## H.264-Specific
- `ProfileLevel` — E, RW. Supported values: Baseline 1.3–5.2, Main 3.0–5.2, High 3.0–5.2, plus AutoLevel variants: Baseline, ConstrainedBaseline, Main, High, ConstrainedHigh, High422, High444Predictive
- `H264EntropyMode` — E, RW *(CAVLC/CABAC)*
- `EnableTransform8x8` — B, RW
- `EnableWeightedPrediction` — B, RW
- `EnableVUIBitstreamRestriction` — B, RW
- `log2_max_minus4` — N, RW
- `UserParameterSetsIds` — N, RW

## Color & Signal Description
- `ColorPrimaries` — E, RW: `ITU_R_709_2`, `EBU_3213`, `SMPTE_C`, `ITU_R_2020`, `P3_D65`, `DCI_P3`
- `TransferFunction` — E, RW: `ITU_R_709_2`, `SMPTE_240M_1995`, `Linear`, `IEC_sRGB`, `ITU_R_2020`, `SMPTE_ST_2084_PQ`, `SMPTE_ST_428_1`, `ITU_R_2100_HLG`, `UseGamma`
- `YCbCrMatrix` — E, RW: `Identity`, `ITU_R_709_2`, `ITU_R_601_4`, `SMPTE_240M_1995`, `ITU_R_2020`, `ITU_R_2100_ICtCp`
- No metadata exposed: `GammaLevel`, `ICCProfile`, `ComponentRange`, `ChromaLocationTopField`, `ChromaLocationBottomField`, `CleanAperture`, `PixelAspectRatio`, `FieldCount`, `FieldDetail`

## HDR Metadata
*(none expose type/status)*
- `AmbientViewingEnvironment`, `ContentLightLevelInfo`, `MasteringDisplayColorVolume`
- `HDRMetadataInsertionMode`, `PreserveDynamicHDRMetadata`
- `InitialHDRMetadataGenerationState`, `CurrentHDRMetadataGenerationState`

## Stereo / Spatial Video
- `EncodesDepth` — B, RW
- `EncodesDisparity` — B, RW
- No metadata exposed: `HasLeftStereoEyeView`, `HasRightStereoEyeView`, `HasEyeViewsReversed`, `HasAdditionalViews`, `HeroEye`, `StereoCameraBaseline`, `HorizontalDisparityAdjustment`, `HorizontalFieldOfView`, `ProjectionKind`, `ViewPackingKind`, `WarpKind`, `CameraCalibrationDataLensCollection`, `AuxiliaryTypeInfo`

## Hardware, Power & Performance
- `UsingHardwareAcceleratedVideoEncoder` — B, RO
- `NumberOfCores` — N, RO
- `MaxEncoderPixelRate` — N, RO
- `MaximizePowerEfficiency` — B, RW
- `ThrottleForBackground` — B, RW
- `Priority` — N, RW
- `EncoderUsage` — N, RW
- `PreemptiveLoadBalancing` — B, RW
- `Paravirtualized` — B, RW
- `RecommendedParallelizationLimit` — N, RO
- `RecommendedParallelizedSubdivisionMinimumDuration` — RO
- `RecommendedParallelizedSubdivisionMinimumFrameCount` — N, RO
- No metadata exposed: `LowMemory`, `FigThreadPriority`

## Input & Pixel Buffers
- `InputPixelFormat` — N, RW
- `InputQueueMaxCount` — N, RW
- `VideoResolutionAdaptation` — B, RW
- `VideoResolutionAdaptationType` — B, RW *(typed Boolean, oddly)*
- `ReconstructedPixelBufferAttributes` — RO
- `MultiPassStorage` — RW
- No metadata exposed: `AllowCompressedPixelFormats`, `AllowPixelTransfer`, `PixelTransferProperties`, `PoolPixelBufferAttributes`, `PoolPixelBufferAttributesSeed`, `PixelBufferPoolIsShared`, `VideoEncoderPixelBufferAttributes`, `PrepareEncodedSampleBuffersForPaddedWrites`

## Motion Estimation
- `MotionEstimationSearchMode` — N, RW
- `SupportedMotionSearchModes` — RO
- `SupportedPresetDictionaries` — RO

## FaceTime / Apple-Internal Knobs
- `EnableUserQPForFacetime` — B, RW
- `EnableUserRefForFacetime` — B, RW
- `UserDPBFramesForFaceTime` — RW
- `iChatUsageString` — E, RW
- `EnableUserQPMap` — N, RW
- `EnableMBInputCtrl` — N, RW
- `ForceRefUncompressed` — B, RW
- `EnsureTIJacinto4Compatibility` — B, RW *(TI Jacinto is an automotive SoC — likely CarPlay-related)*

## Diagnostics, Stats & Session Identity
- `CalculateMeanSquaredError` — B, RW
- `EnableStatsCollect` — N, RW
- `DebugMetadataSEI` — B, RW
- `SessionName` — E, RW
- No metadata exposed: `NumberOfPendingFrames`, `PowerLogSessionID`, `ClientPID`, `EncoderID`, `TransportIdentifier`, `UsingMetalRegistryID`
