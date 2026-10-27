#include "pet_config.h"
#include "sd_probe.h"

/* PET_SD_PROBE=0：跳过 210 轮引脚扫描，直接返回无卡。
 * 诊断时需要扫描 TF 卡，把下面函数体换回完整版（git 历史里有）。 */
bool PetSdCardProbe() {
    return false;
}
