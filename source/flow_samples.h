#pragma once
#include "flow_model.h"
QStringList flowSampleNames();
QString flowSampleHint(int index);
QVector<FlowStep> flowSampleSteps(int index, bool ocr);
bool flowSamplesSelfTest();
