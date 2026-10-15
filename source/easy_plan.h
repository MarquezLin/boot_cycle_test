#pragma once
#include "flow_model.h"
struct EasyPlan {
    bool running = false, resolution = false, fps = false, content = false, motion = false, audio = false, video = false, clickStart = false;
    bool textCompare = false;
    bool delay = false;
    int delayMs = 1000;
    QString expectedText, textCondition = "equalsIgnoreCase";
    int width = 1920, height = 1080;
    double targetFps = 60, tolerance = 1;
};
QVector<FlowStep> easyPlanSteps(const EasyPlan &plan);
QString describeEasyPlan(const EasyPlan &plan);
bool easyPlanSelfTest();
