#include <vitasdkkern.h>
#include <taihen.h>
#include <stdbool.h>
#include <string.h>

#include "main.h"
#include "oc.h"
#include "gui.h"
#include "perf.h"

int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr);
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
bool ksceAppMgrIsExclusiveProcessRunning();

#define PSVS_MAX_HOOKS 14
static tai_hook_ref_t g_hookrefs[PSVS_MAX_HOOKS];
static SceUID         g_hooks[PSVS_MAX_HOOKS];
static SceUID         g_injects[1];

static SceUID g_mutex_cpufreq_uid = -1;
static SceUID g_thread_uid = -1;
static bool   g_thread_run = true;

char g_titleid[32] = "";

int (*SceSysmemForKernel_0x3650963F)(uint32_t a1, SceSysmemAddressSpaceInfo *a2);
int (*SceThreadmgrForDriver_0x7E280B69)(SceKernelSystemInfo *pInfo);
int (*ScePervasiveForDriver_0xE9D95643)(int mul, int ndiv);

uint32_t *ScePower_41C8 = NULL;
uint32_t *ScePower_41CC = NULL;

int (*_kscePowerGetArmClockFrequency)();
int (*_kscePowerGetBusClockFrequency)();
int (*_kscePowerGetGpuClockFrequency)();
int (*_kscePowerGetGpuEs4ClockFrequency)(int *a1, int *a2);
int (*_kscePowerGetGpuXbarClockFrequency)();

int (*_kscePowerSetArmClockFrequency)(int freq);
int (*_kscePowerSetBusClockFrequency)(int freq);
int (*_kscePowerSetGpuClockFrequency)(int freq);
int (*_kscePowerSetGpuEs4ClockFrequency)(int a1, int a2);
int (*_kscePowerSetGpuXbarClockFrequency)(int freq);

static void psvs_input_check(SceCtrlData *pad_data, int count) {
    // Do not pass input to fg app
    if (psvs_gui_get_mode() == PSVS_GUI_MODE_FULL) {
        SceCtrlData kctrl;
        kctrl.buttons = 0;
        for (int i = 0; i < count; i++)
            ksceKernelMemcpyKernelToUser((uintptr_t)&pad_data[i].buttons, &kctrl.buttons, sizeof(uint32_t));
    }
}

int ksceDisplaySetFrameBufInternal_patched(int head, int index, const SceDisplayFrameBuf *pParam, int sync) {
    if (!head || !pParam)
        goto DISPLAY_HOOK_RET;

    if (index && ksceAppMgrIsExclusiveProcessRunning())
        goto DISPLAY_HOOK_RET; // Do not draw over SceShell overlay

    // Check buttons
    SceCtrlData kctrl;
    int ret = ksceCtrlPeekBufferPositive(0, &kctrl, 1);
    if (ret < 0)
        ret = ksceCtrlPeekBufferPositive(1, &kctrl, 1);
    if (ret > 0)
        psvs_gui_input_check(kctrl.buttons);

    if (psvs_gui_get_mode() == PSVS_GUI_MODE_HIDDEN)
        goto DISPLAY_HOOK_RET;

    psvs_perf_calc_fps();
    psvs_gui_set_framebuf(pParam);

    if (psvs_gui_get_mode() == PSVS_GUI_MODE_FULL) {
        ksceKernelGetProcessTitleId(ksceKernelGetProcessId(), g_titleid, sizeof(g_titleid));
        psvs_perf_poll_memory();
    }

    if (psvs_gui_get_mode() == PSVS_GUI_MODE_FPS) {
        // draw onto fb
        psvs_gui_dd_fps();
    } else {
        // cpy from buffer
        psvs_gui_cpy();
    }

DISPLAY_HOOK_RET:
    return TAI_CONTINUE(int, g_hookrefs[0], head, index, pParam, sync);
}

DECL_FUNC_HOOK_PATCH_CTRL(1, sceCtrlPeekBufferNegative)
DECL_FUNC_HOOK_PATCH_CTRL(2, sceCtrlPeekBufferNegative2)
DECL_FUNC_HOOK_PATCH_CTRL(3, sceCtrlPeekBufferPositive)
DECL_FUNC_HOOK_PATCH_CTRL(4, sceCtrlPeekBufferPositive2)
DECL_FUNC_HOOK_PATCH_CTRL(5, sceCtrlReadBufferNegative)
DECL_FUNC_HOOK_PATCH_CTRL(6, sceCtrlReadBufferNegative2)
DECL_FUNC_HOOK_PATCH_CTRL(7, sceCtrlReadBufferPositive)
DECL_FUNC_HOOK_PATCH_CTRL(8, sceCtrlReadBufferPositive2)

int kscePowerSetArmClockFrequency_patched(int freq) {
    int ret = ksceKernelLockMutex(g_mutex_cpufreq_uid, 1, NULL);
    if (ret < 0)
        return ret;

    freq = psvs_oc_get_freq(PSVS_OC_CPU, freq);
    ret = TAI_CONTINUE(int, g_hookrefs[9], freq);

    if (freq > 444 && freq <= 500) {
        psvs_oc_holy_shit();
        ret = 0;
    }

    ksceKernelUnlockMutex(g_mutex_cpufreq_uid, 1);
    return ret;
}

int kscePowerSetBusClockFrequency_patched(int freq) {
    return TAI_CONTINUE(int, g_hookrefs[10], psvs_oc_get_freq(PSVS_OC_BUS, freq));
}

int kscePowerSetGpuClockFrequency_patched(int freq) {
    return TAI_CONTINUE(int, g_hookrefs[11], psvs_oc_get_freq(PSVS_OC_GPU, freq));
}

int kscePowerSetGpuEs4ClockFrequency_patched(int a1, int a2) {
    a1 = psvs_oc_get_freq(PSVS_OC_GPU_ES4, a1);
    a2 = psvs_oc_get_freq(PSVS_OC_GPU_ES4, a2);
    return TAI_CONTINUE(int, g_hookrefs[12], a1, a2);
}

int kscePowerSetGpuXbarClockFrequency_patched(int freq) {
    return TAI_CONTINUE(int, g_hookrefs[13], psvs_oc_get_freq(PSVS_OC_GPU_XBAR, freq));
}

static int psvs_thread(SceSize args, void *argp) {
    while (g_thread_run) {
        // If in OSD/FULL mode, poll shown info
        if (psvs_gui_get_mode() >= PSVS_GUI_MODE_OSD) {
            psvs_perf_poll_cpu();
        }

        // Redraw buffer template on gui mode or fb change
        if (psvs_gui_mode_changed() || psvs_gui_fb_res_changed()) {
            if (psvs_gui_get_mode() == PSVS_GUI_MODE_OSD) {
                psvs_gui_draw_osd_template();
            } else if (psvs_gui_get_mode() == PSVS_GUI_MODE_FULL) {
                psvs_gui_draw_template();
            }
        }

        // Draw OSD mode
        if (psvs_gui_get_mode() == PSVS_GUI_MODE_OSD) {
            psvs_gui_draw_osd_cpu();
            psvs_gui_draw_osd_fps();
        }

        // Draw FULL mode
        if (psvs_gui_get_mode() == PSVS_GUI_MODE_FULL) {
            psvs_gui_draw_header();
            psvs_gui_draw_cpu_section();
            psvs_gui_draw_memory_section();
            psvs_gui_draw_menu();
        }

        ksceKernelDelayThread(50 * 1000);
    }

    return 0;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
    int ret = 0;
    psvs_gui_init();

    tai_module_info_t tai_info;
    tai_info.size = sizeof(tai_module_info_t);
    taiGetModuleInfoForKernel(KERNEL_PID, "ScePower", &tai_info);

    module_get_offset(KERNEL_PID, tai_info.modid, 1, 0x41C8, (uintptr_t *)&ScePower_41C8);
    module_get_offset(KERNEL_PID, tai_info.modid, 1, 0x41CC, (uintptr_t *)&ScePower_41CC);

    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0xABC6F88F, (uintptr_t *)&_kscePowerGetArmClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x478FE6F5, (uintptr_t *)&_kscePowerGetBusClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x64641E6A, (uintptr_t *)&_kscePowerGetGpuClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x475BCC82, (uintptr_t *)&_kscePowerGetGpuEs4ClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x0A750DEE, (uintptr_t *)&_kscePowerGetGpuXbarClockFrequency);

    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x74DB5AE5, (uintptr_t *)&_kscePowerSetArmClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0xB8D7B3FB, (uintptr_t *)&_kscePowerSetBusClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x621BD8FD, (uintptr_t *)&_kscePowerSetGpuClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0x264C24FC, (uintptr_t *)&_kscePowerSetGpuEs4ClockFrequency);
    module_get_export_func(KERNEL_PID,
            "ScePower", 0x1590166F, 0xA7739DBE, (uintptr_t *)&_kscePowerSetGpuXbarClockFrequency);

    g_mutex_cpufreq_uid = ksceKernelCreateMutex("psvs_mutex_cpufreq", 0, 0, NULL);

    psvs_oc_init();

    g_hooks[0] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[0],
            "SceDisplay", 0x9FED47AC, 0x16466675, ksceDisplaySetFrameBufInternal_patched);

    g_hooks[1] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[1],
            "SceCtrl", 0xD197E3C7, 0x104ED1A7, sceCtrlPeekBufferNegative_patched);
    g_hooks[2] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[2],
            "SceCtrl", 0xD197E3C7, 0x81A89660, sceCtrlPeekBufferNegative2_patched);
    g_hooks[3] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[3],
            "SceCtrl", 0xD197E3C7, 0xA9C3CED6, sceCtrlPeekBufferPositive_patched);
    g_hooks[4] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[4],
            "SceCtrl", 0xD197E3C7, 0x15F81E8C, sceCtrlPeekBufferPositive2_patched);
    g_hooks[5] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[5],
            "SceCtrl", 0xD197E3C7, 0x15F96FB0, sceCtrlReadBufferNegative_patched);
    g_hooks[6] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[6],
            "SceCtrl", 0xD197E3C7, 0x27A0C5FB, sceCtrlReadBufferNegative2_patched);
    g_hooks[7] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[7],
            "SceCtrl", 0xD197E3C7, 0x67E7AB83, sceCtrlReadBufferPositive_patched);
    g_hooks[8] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[8],
            "SceCtrl", 0xD197E3C7, 0xC4226A3E, sceCtrlReadBufferPositive2_patched);

    g_hooks[9] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[9],
            "ScePower", 0x1590166F, 0x74DB5AE5, kscePowerSetArmClockFrequency_patched);
    g_hooks[10] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[10],
            "ScePower", 0x1590166F, 0xB8D7B3FB, kscePowerSetBusClockFrequency_patched);
    g_hooks[11] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[11],
            "ScePower", 0x1590166F, 0x621BD8FD, kscePowerSetGpuClockFrequency_patched);
    g_hooks[12] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[12],
            "ScePower", 0x1590166F, 0x264C24FC, kscePowerSetGpuEs4ClockFrequency_patched);
    g_hooks[13] = taiHookFunctionExportForKernel(KERNEL_PID, &g_hookrefs[13],
            "ScePower", 0x1590166F, 0xA7739DBE, kscePowerSetGpuXbarClockFrequency_patched);

    ret = module_get_export_func(KERNEL_PID,
            "SceSysmem", 0x63A519E5, 0x3650963F, (uintptr_t *)&SceSysmemForKernel_0x3650963F); // 3.60
    if (ret < 0) {
        module_get_export_func(KERNEL_PID,
            "SceSysmem", 0x02451F0F, 0xB9B69700, (uintptr_t *)&SceSysmemForKernel_0x3650963F); // 3.65
    }
    module_get_export_func(KERNEL_PID,
            "SceKernelThreadMgr", 0xE2C40624, 0x7E280B69, (uintptr_t *)&SceThreadmgrForDriver_0x7E280B69);
    module_get_export_func(KERNEL_PID,
            "SceLowio", 0xE692C727, 0xE9D95643, (uintptr_t *)&ScePervasiveForDriver_0xE9D95643);

    const uint8_t nop[] = {0x00, 0xBF};
    g_injects[0] = taiInjectAbsForKernel(KERNEL_PID,
            (void *)((uintptr_t)ScePervasiveForDriver_0xE9D95643 + 0x1D), &nop, 2);

    g_thread_uid = ksceKernelCreateThread("psvs_thread", psvs_thread, 0x3C, 0x3000, 0, 0x10000, 0);
    ksceKernelStartThread(g_thread_uid, 0, NULL);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    if (g_thread_uid >= 0) {
        g_thread_run = 0;
        ksceKernelWaitThreadEnd(g_thread_uid, NULL, NULL);
        ksceKernelDeleteThread(g_thread_uid);
    }

    for (int i = 0; i < PSVS_MAX_HOOKS; i++) {
        if (g_hooks[i] >= 0)
            taiHookReleaseForKernel(g_hooks[i], g_hookrefs[i]);
    }

    if (g_injects[0] >= 0) {
        taiInjectReleaseForKernel(g_injects[0]);
    }

    if (g_mutex_cpufreq_uid >= 0) {
        ksceKernelDeleteMutex(g_mutex_cpufreq_uid);
    }

    psvs_gui_deinit();

    return SCE_KERNEL_STOP_SUCCESS;
}
