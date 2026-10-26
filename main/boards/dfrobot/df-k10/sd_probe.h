#pragma once

/*
 * TF 卡引脚探测。
 *
 * 为什么要有这个：UNIHIKER K10 的 microSD 卡座是自弹式的，硬件确实有，
 * 但 DFRobot 公开的规格页和 wiki 都没给 SD 卡的 GPIO 分配（社区里甚至有人
 * 误传「K10 没有卡槽」）。与其猜，不如让设备自己遍历候选引脚挂载一遍，
 * 挂上了就把真实引脚打进串口日志。
 *
 * 只在 PET_SD_PROBE=1 时编译；探测本身约 1 分钟，只在诊断固件里开。
 */
bool PetSdCardProbe();
