#ifndef OF_PC

#include "of_file.h"
#include "of_mount.h"
#include "of_services.h"
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

static void of_ecwolf_register_base_file_aliases(const char *extension)
{
    static const char *const names[] = {
        "AUDIOHED", "AUDIOT", "GAMEMAPS", "MAPHEAD",
        "VGADICT", "VGAGRAPH", "VGAHEAD", "VSWAP"
    };
    static const uint32_t slots[] = {
        /* GAMEMAPS moved from slot 9 to 25: a data.json slot between the
         * Shared Config (8) and the saves (10-19) shifts the APF datatable
         * entry positions that the kernel map and the RTL SAVE_DT size
         * commit assume, which silently corrupted save-file sizes. */
        5, 6, 25, 20, 21, 22, 23, 24
    };

    char filename[24];
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        snprintf(filename, sizeof(filename), "%s.%s", names[i], extension);
        of_file_slot_register(slots[i], filename);
    }
}

static void of_ecwolf_set_data_extension(const char *extension)
{
    setenv("ECWOLF_DATA_EXT", extension, 1);
    printf("OpenFPGA: selecting ECWolf data extension .%s.\n", extension);
}

static void of_ecwolf_select_data_extension(void)
{
    const char *current = getenv("ECWOLF_DATA_EXT");
    if (current && current[0])
        return;

    /* The game data files are referenced directly as data slots (not a mounted
     * CUE/BIN), so pick the ECWolf data extension from whichever VSWAP variant
     * the instance bound. */
    uint32_t slot = 0;
    if (of_ecwolf_slot_matches(2, "spear-m2.ini"))
    {
        of_ecwolf_register_base_file_aliases("SD2");
        of_ecwolf_set_data_extension("sd2");
    }
    else if (of_ecwolf_slot_matches(2, "spear-m3.ini"))
    {
        of_ecwolf_register_base_file_aliases("SD3");
        of_ecwolf_set_data_extension("sd3");
    }
    else if (of_file_slot_find("VSWAP.WL6", &slot) == 0)
    {
        of_ecwolf_register_base_file_aliases("WL6");
        of_ecwolf_set_data_extension("wl6");
    }
    else if (of_file_slot_find("VSWAP.SOD", &slot) == 0)
    {
        of_ecwolf_register_base_file_aliases("SOD");
        of_ecwolf_set_data_extension("sod");
    }
    else if (of_file_slot_find("VSWAP.SDM", &slot) == 0)
    {
        of_ecwolf_register_base_file_aliases("SDM");
        of_ecwolf_set_data_extension("sdm");
    }
    else if (of_file_slot_find("VSWAP.SD2", &slot) == 0)
    {
        of_ecwolf_register_base_file_aliases("SD2");
        of_ecwolf_set_data_extension("sd2");
    }
    else if (of_file_slot_find("VSWAP.SD3", &slot) == 0)
    {
        of_ecwolf_register_base_file_aliases("SD3");
        of_ecwolf_set_data_extension("sd3");
    }
    else if (of_file_slot_find("VSWAP.N3D", &slot) == 0)
    {
        of_ecwolf_register_base_file_aliases("N3D");
        of_ecwolf_set_data_extension("n3d");
    }
}

static void of_ecwolf_log_bank_status(void)
{
    if (!OF_SVC || OF_SVC->magic != OF_SVC_MAGIC)
    {
        printf("OFSF: service table unavailable.\n");
        return;
    }

    const ofsf_header_t *hdr = (const ofsf_header_t *)OF_SVC->smp_bank_preload_base;
    uint32_t size = OF_SVC->smp_bank_preload_size;
    if (!hdr || size < sizeof(*hdr))
    {
        printf("OFSF: bank.ofsf was not preloaded.\n");
        return;
    }

    if (hdr->magic != OFSF_MAGIC || hdr->version != OFSF_VERSION ||
        hdr->sample_data_offset > size ||
        hdr->sample_data_size > size - hdr->sample_data_offset)
    {
        printf("OFSF: bank.ofsf preload header is invalid.\n");
        return;
    }

    printf("OFSF: preloaded bank %.32s (%lu zones, %lu sample bytes).\n",
           hdr->bank_name,
           (unsigned long)hdr->zone_count,
           (unsigned long)hdr->sample_data_size);
}

__attribute__((constructor(200)))
static void of_ecwolf_openfpga_init(void)
{
    char name[16];

    of_ecwolf_setenv_default("HOME", "/");
    of_ecwolf_setenv_default("XDG_CONFIG_HOME", "/");
    of_ecwolf_setenv_default("XDG_DATA_HOME", "/");
    of_ecwolf_select_data_extension();

    of_file_slot_register(4, "wolfmidi.zip");
    of_file_slot_register(7, "bank.ofsf");
    of_ecwolf_log_bank_status();
    of_file_slot_register(8, "ecwolf.cfg");
    /* Host-rendered AdLib SFX cache (scripts/sfxcache.sh); optional. */
    of_file_slot_register(26, "sfxcache.ofx");
    /* No disc image to mount: the game's AUDIOHED/AUDIOT/GAMEMAPS/MAPHEAD/
     * VGADICT/VGAGRAPH/VGAHEAD/VSWAP files are bound to data slots by the
     * instance and auto-discovered, so ECWolf finds them directly in the
     * virtual root ($PROGDIR) instead of via the /cd ISO bridge. */

    for (int i = 0; i < 10; i++)
    {
        snprintf(name, sizeof(name), "savegam%d.sav", i);
        of_file_slot_register((uint32_t)(10 + i), name);
    }
}

#else
typedef int of_ecwolf_openfpga_unused_on_pc;
#endif
