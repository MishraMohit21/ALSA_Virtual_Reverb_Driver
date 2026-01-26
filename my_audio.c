#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mohit Mishra");
MODULE_DESCRIPTION("Kernel Level Reverb Driver");

static struct platform_device *my_pdev;
static struct snd_card *my_reverb_card;
static struct snd_pcm *my_reverb_pcm;

static int playback_count = 1;
static int capture_count = 1;




static const struct snd_pcm_hardware my_pcm_hardware = {
    .info = (SNDRV_PCM_INFO_MMAP |
             SNDRV_PCM_INFO_MMAP_VALID |
             SNDRV_PCM_INFO_INTERLEAVED |
             SNDRV_PCM_INFO_BLOCK_TRANSFER),
    
    .formats = SNDRV_PCM_FMTBIT_S16_LE,      // Only 16-bit
    .rates = SNDRV_PCM_RATE_44100,           // Only 44.1kHz
    .rate_min = 44100,
    .rate_max = 44100,
    .channels_min = 2,                       // Stereo
    .channels_max = 2,
    .buffer_bytes_max = 64 * 1024,           // Max Buffer (64KB)
    .period_bytes_min = 1024,                // Min Chunk Size
    .period_bytes_max = 32 * 1024,           // Max Chunk Size
    .periods_min = 2,                        // At least 2 chunks (Ping-Pong)
    .periods_max = 1024,
};


//////////////////////////
// REGISTER THE PLATFORM /
//////////////////////////
static int echo_register_platform_device(void)
{
    my_pdev = platform_device_register_simple(
        "my_reverb_device", -1, NULL, 0);

    if (IS_ERR(my_pdev)) {
        printk(KERN_ERR "EchoDriver: Failed to register platform device\n");
        return PTR_ERR(my_pdev);
    }

    return 0;
}



//////////////////////////
// CREATE THE CARD     /
//////////////////////////
static int echo_create_card(void)
{
    int err;

    err = snd_card_new(&my_pdev->dev, -1, "EchoCard",
                       THIS_MODULE, 0, &my_reverb_card);
    if (err < 0) {
        printk(KERN_ERR
               "EchoDriver: Failed to create card (Err: %d)\n", err);
        return err;
    }

    strcpy(my_reverb_card->driver, "EchoDriver");
    strcpy(my_reverb_card->shortname, "Echo");
    strcpy(my_reverb_card->longname,
           "Mohit's Experimental Reverb");

    return 0;
}


//////////////////////////
// REGISTER THE CARD     /
//////////////////////////
static int echo_register_card(void)
{
    int err;

    err = snd_card_register(my_reverb_card);
    if (err < 0) {
        printk(KERN_ERR
               "EchoDriver: Failed to register card (Err: %d)\n", err);
        return err;
    }

    return 0;
}



static int my_pcm_open(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;

    runtime->hw = my_pcm_hardware;

    printk(KERN_INFO "EchoDriver: PCM Device Opened\n");
    return 0; 
}

static int my_pcm_close(struct snd_pcm_substream *substream)
{
    printk(KERN_INFO "EchoDriver: PCM Device Closed\n");
    return 0;
}

// 1. HW PARAMS: "I accept these settings"
static int my_pcm_hw_params(struct snd_pcm_substream *substream,
                            struct snd_pcm_hw_params *params)
{
    // usually we allocate memory here, but we used "Managed Buffer"
    // so we don't need to do anything!
    return 0;
}

// 2. PREPARE: "I am ready to start"
static int my_pcm_prepare(struct snd_pcm_substream *substream)
{
    // Usually we reset the hardware ring buffer here
    return 0;
}

// 3. TRIGGER: "Start/Stop the audio!"
static int my_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    // We will handle START and STOP commands here later.
    return 0;
}

// 4. POINTER: "Where is the read head?"
static snd_pcm_uframes_t my_pcm_pointer(struct snd_pcm_substream *substream)
{
    // For now, lie and say we are at position 0.
    // Later we will return the real buffer position.
    return 0;
}


static const struct snd_pcm_ops my_reverb_ops  = {

    .open = my_pcm_open,
    .close = my_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params =  my_pcm_hw_params, // <--- Added
    .prepare =    my_pcm_prepare,   // <--- Added
    .trigger =    my_pcm_trigger,   // <--- Added
    .pointer =    my_pcm_pointer,   // <--- Added
};



//////////////////////////
// CREATE THE PCM DEVICE /
//////////////////////////
static int echo_create_pcm(void)
{
    int err; 

    err = snd_pcm_new(my_reverb_card, "Reverb_PCM", 0, 
                                playback_count, capture_count, &my_reverb_pcm);
    if (err < 0) {
        printk(KERN_ERR
               "EchoDriver: Failed to create PCM (Err: %d)\n", err);
        return err;
    }
  
    // Link the PCM OPS
    snd_pcm_set_ops(my_reverb_pcm, SNDRV_PCM_STREAM_PLAYBACK, &my_reverb_ops);
    snd_pcm_set_ops(my_reverb_pcm, SNDRV_PCM_STREAM_CAPTURE, &my_reverb_ops);
  
    snd_pcm_set_managed_buffer_all(my_reverb_pcm,
                               SNDRV_DMA_TYPE_VMALLOC, // Use vmalloc memory
                               NULL,                   // No specific device
                               64 * 1024,              // Default size
                               64 * 1024);             // Max size

    return 0;
}

static int __init echo_probe(void)
{
    int err;

    printk(KERN_INFO "EchoDriver: initializing...\n");

    err = echo_register_platform_device();
    if (err)
        return err;

    err = echo_create_card();
    if (err)
        goto err_pdev;

    err = echo_create_pcm();
    if (err)
        goto err_card; // If engine fails, we must destroy the card

    err = echo_register_card();
    if (err)
        goto err_card;

    printk(KERN_INFO "EchoDriver: Loaded successfully!\n");
    return 0;

    err_card:
        snd_card_free(my_reverb_card);
    err_pdev:
        platform_device_unregister(my_pdev);
    
    
    return err;
}

static void __exit echo_exit(void) 
{
    if (my_reverb_card) {
        snd_card_free(my_reverb_card); 
    }
    if (my_pdev) {
        platform_device_unregister(my_pdev);
    }
    printk(KERN_INFO "EchoDriver: Unloaded.\n");
}

module_init(echo_probe); 
module_exit(echo_exit);