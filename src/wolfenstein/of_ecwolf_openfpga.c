#ifndef OF_PC

#include "of_file.h"
#include "of_mount.h"
#include "of_smp_bank.h"

#include <stdio.h>
#include <stdlib.h>

static void of_ecwolf_setenv_default(const char *name, const char *value)
{
    const char *current = getenv(name);
    if (!current || !current[0])
        setenv(name, value, 1);
}

static int of_ecwolf_slot_matches(uint32_t expected_slot, const char *filename)
{
    uint32_t slot = 0;
    return of_file_slot_find(filename, &slot) == 0 && slot == expected_slot;
}

static void of_ecwolf_select_data_extension(void)
{
    const char *current = getenv("ECWOLF_DATA_EXT");
    if (current && current[0])
        return;

    if (of_ecwolf_slot_matches(14, "Wolfenstein3D.cue") ||
        of_ecwolf_slot_matches(15, "Wolfenstein3D.bin"))
    {
        setenv("ECWOLF_DATA_EXT", "wl6", 1);
        printf("OpenFPGA: selecting ECWolf data extension .wl6.\n");
    }
    else if (of_ecwolf_slot_matches(14, "Spear of Destiny (USA).cue") ||
             of_ecwolf_slot_matches(15, "Spear of Destiny (USA).bin"))
    {
        setenv("ECWOLF_DATA_EXT", "sod", 1);
        printf("OpenFPGA: selecting ECWolf data extension .sod.\n");
    }
}

static void of_ecwolf_log_bank_status(void)
{
    int rc = of_smp_bank_bind_preloaded();
    if (rc > 0)
    {
        const ofsf_header_t *hdr = of_smp_bank_get();
        if (hdr)
        {
            printf("OFSF: loaded bank %.32s (%lu zones, %lu sample bytes).\n",
                   hdr->bank_name,
                   (unsigned long)hdr->zone_count,
                   (unsigned long)hdr->sample_data_size);
        }
        else
        {
            printf("OFSF: loaded bank.\n");
        }
    }
    else if (rc == 0)
    {
        printf("OFSF: bank.ofsf was not preloaded.\n");
    }
    else
    {
        printf("OFSF: bank.ofsf preload failed (%d).\n", rc);
    }
}

__attribute__((constructor(200)))
static void of_ecwolf_openfpga_init(void)
{
    char name[16];

    of_ecwolf_setenv_default("HOME", "/");
    of_ecwolf_setenv_default("XDG_CONFIG_HOME", "/");
    of_ecwolf_setenv_default("XDG_DATA_HOME", "/");
    of_ecwolf_select_data_extension();

    of_file_slot_register(4, "bank.ofsf");
    of_ecwolf_log_bank_status();
    of_file_slot_register(20, "ecwolf.cfg");
    if (of_iso_mount("slot:14", "/cd") < 0)
        of_iso_mount("slot:15", "/cd");

    for (int i = 0; i < 10; i++)
    {
        snprintf(name, sizeof(name), "savegam%d.ecs", i);
        of_file_slot_register((uint32_t)(21 + i), name);
    }
}

#else
typedef int of_ecwolf_openfpga_unused_on_pc;
#endif
