// SPDX-License-Identifier: GPL-2.0-only
// Skeleton driver for Guillemot Maxi Studio ISIS (ESS Maestro-2E)

#include <linux/init.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/ac97_codec.h>
#include <sound/mpu401.h>

#define DRIVER_NAME "maestro2"
#define VENDOR_ID    0x125D
#define DEVICE_ID    0x1978

/* I/O offsets from BAR0 */
#define ESM_INDEX         0x02
#define ESM_DATA          0x00
#define ESM_AC97_INDEX    0x30
#define ESM_AC97_DATA     0x32
#define ESM_MPU401_PORT   0x98
#define ESM_HOST_IRQ      0x18

struct maestro2 {
    struct snd_card *card;
    struct pci_dev *pci;
    unsigned long iobase;
    int irq;
    struct snd_ac97 *ac97;
    struct snd_rawmidi *rmidi;
    const struct firmware *fw;
    spinlock_t reg_lock;
};

/* Register access helpers */
static inline u16 maestro2_read(struct maestro2 *chip, u16 reg)
{
    outw(reg, chip->iobase + ESM_INDEX);
    return inw(chip->iobase + ESM_DATA);
}

static inline void maestro2_write(struct maestro2 *chip, u16 reg, u16 val)
{
    outw(reg, chip->iobase + ESM_INDEX);
    outw(val, chip->iobase + ESM_DATA);
}

/* AC'97 ops */
static void maestro2_ac97_write(struct snd_ac97 *ac97, unsigned short reg,
                                unsigned short val)
{
    struct maestro2 *chip = ac97->private_data;
    outw(val, chip->iobase + ESM_AC97_DATA);
    outb(reg, chip->iobase + ESM_AC97_INDEX);
    mdelay(1);
}

static unsigned short maestro2_ac97_read(struct snd_ac97 *ac97,
                                         unsigned short reg)
{
    struct maestro2 *chip = ac97->private_data;
    outb(reg | 0x80, chip->iobase + ESM_AC97_INDEX);
    mdelay(1);
    return inw(chip->iobase + ESM_AC97_DATA);
}

static struct snd_ac97_bus_ops maestro2_ac97_ops = {
    .write = maestro2_ac97_write,
    .read  = maestro2_ac97_read,
};

static int maestro2_ac97_init(struct maestro2 *chip)
{
    struct snd_ac97_bus *bus;
    struct snd_ac97_template ac97;
    int err;

    err = snd_ac97_bus(chip->card, 0, &maestro2_ac97_ops, chip, &bus);
    if (err < 0)
        return err;

    memset(&ac97, 0, sizeof(ac97));
    ac97.private_data = chip;
    ac97.pci = chip->pci;

    err = snd_ac97_mixer(bus, &ac97, &chip->ac97);
    if (err < 0)
        return err;

    dev_info(&chip->pci->dev, "AC'97 codec id 0x%x\n", chip->ac97->id);
    return 0;
}

static int maestro2_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct snd_card *card;
    struct maestro2 *chip;
    int err;

    err = snd_devm_card_new(&pdev->dev, -1, NULL, THIS_MODULE,
                            sizeof(*chip), &card);
    if (err < 0)
        return err;
    chip = card->private_data;
    chip->card = card;
    chip->pci = pdev;
    spin_lock_init(&chip->reg_lock);

    err = pci_enable_device(pdev);
    if (err < 0)
        return err;
    pci_set_master(pdev);

    chip->iobase = pci_resource_start(pdev, 0);
    chip->irq = pdev->irq;

    if (!devm_request_region(&pdev->dev, chip->iobase,
                             pci_resource_len(pdev, 0), DRIVER_NAME)) {
        dev_err(&pdev->dev, "I/O region already in use\n");
        return -EBUSY;
    }

    err = maestro2_ac97_init(chip);
    if (err < 0) {
        dev_err(&pdev->dev, "AC'97 init failed\n");
        return err;
    }

    err = snd_mpu401_uart_new(card, 0, MPU401_HW_MPU401,
                              chip->iobase + ESM_MPU401_PORT, 0,
                              chip->irq, NULL, &chip->rmidi);
    if (err < 0)
        dev_warn(&pdev->dev, "MPU-401 init failed\n");

    /* Firmware is optional for basic audio */
    if (request_firmware(&chip->fw, "pci64.bin", &pdev->dev) == 0)
        dev_info(&pdev->dev, "Firmware loaded\n");
    else
        dev_warn(&pdev->dev, "Firmware pci64.bin not found\n");

    strcpy(card->driver, DRIVER_NAME);
    strcpy(card->shortname, "Guillemot Maxi Studio ISIS");
    snprintf(card->longname, sizeof(card->longname),
             "%s at 0x%lx, irq %d", card->shortname, chip->iobase, chip->irq);

    err = snd_card_register(card);
    if (err < 0)
        return err;

    pci_set_drvdata(pdev, card);
    return 0;
}

static void maestro2_remove(struct pci_dev *pdev)
{
    struct snd_card *card = pci_get_drvdata(pdev);
    struct maestro2 *chip = card->private_data;

    release_firmware(chip->fw);
    snd_card_free(card);
}

static const struct pci_device_id maestro2_ids[] = {
    { PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, maestro2_ids);

static struct pci_driver maestro2_driver = {
    .name = DRIVER_NAME,
    .id_table = maestro2_ids,
    .probe = maestro2_probe,
    .remove = maestro2_remove,
};

module_pci_driver(maestro2_driver);
MODULE_LICENSE("GPL");
