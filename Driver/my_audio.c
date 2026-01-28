#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>    // <--- REQUIRED for kzalloc/kfree
#include <linux/timer.h>   // <--- REQUIRED for timer_setup/mod_timer
#include <linux/jiffies.h> // <--- REQUIRED for jiffies/msecs_to_jiffies
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


struct echo_runtime {
    struct snd_pcm_substream *substream; // Reference to the stream
    struct timer_list timer;             // The Heartbeat
    unsigned long hw_ptr;                // Current Position
    int period_size;                     // Bytes per chunk
};

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



// static void process_audio_chunk(struct echo_runtime *chip)
// {
//     struct snd_pcm_runtime *runtime = chip->substream->runtime;
//     short *buffer = (short *)runtime->dma_area;
    
//     // 1. Setup Iteration
//     int frames = runtime->rate / 100; // 10ms worth of data
//     int current_idx = chip->hw_ptr;
//     int buffer_size = runtime->buffer_size; // Total size in frames
    
//     int i;
//     for (i = 0; i < frames; i++) {
//         int pos = (current_idx + i) % buffer_size;
        
//         int past_pos = pos - 4000;
//         if (past_pos < 0) past_pos += buffer_size; // Wrap around
        
//         // B. Read Values
//         short input_sample = buffer[pos];
//         short echo_sample = buffer[past_pos];
        
//         short mixed_sample = input_sample + (echo_sample >> 1);
        
//         // D. Write back to memory
//         buffer[pos] = mixed_sample;
        
//         if (i == 0 && abs(input_sample) > 1000) {
//              if (jiffies % 50 == 0) {
//                  printk(KERN_INFO "Echo: In=%d  +  Old=%d  ->  Out=%d\n", 
//                         input_sample, echo_sample, mixed_sample);
//              }
//         }
//     }
// }
static void process_audio_chunk(struct echo_runtime *chip)
{
    struct snd_pcm_runtime *runtime = chip->substream->runtime;
    
    short *buffer = (short *)runtime->dma_area;
    
    int current_index = chip->hw_ptr; 
    
    short sample_value = buffer[current_index];

    if (abs(sample_value) > 500) {
        if (jiffies % 50 == 0) { 
            printk(KERN_INFO "EchoDriver: I see sound! Level = %d\n", sample_value);
        }
    }
}


static void echo_timer_function(struct timer_list *t)
{
    // Recover chip from timer
    struct echo_runtime *chip = from_timer(chip, t, timer);
    struct snd_pcm_substream *substream = chip->substream;
    struct snd_pcm_runtime *runtime = substream->runtime;

    int frames_to_process = runtime->rate / 100;

    process_audio_chunk(chip);

    // Move pointer forward
    chip->hw_ptr += frames_to_process;
    
    if (chip->hw_ptr >= runtime->buffer_size)
        chip->hw_ptr -= runtime->buffer_size;

    // Notify ALSA
    snd_pcm_period_elapsed(substream);

    // Restart timer (10ms)
    mod_timer(&chip->timer, jiffies + msecs_to_jiffies(10));
}

static snd_pcm_uframes_t my_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip = substream->runtime->private_data;
    return bytes_to_frames(substream->runtime, chip->hw_ptr);
}

// [INSERT BEFORE OPEN] The Trigger Callback
static int my_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct echo_runtime *chip = substream->runtime->private_data;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        mod_timer(&chip->timer, jiffies + msecs_to_jiffies(10));
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        del_timer(&chip->timer);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int my_pcm_open(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip;
    struct snd_pcm_runtime *runtime = substream->runtime;

    runtime->hw = my_pcm_hardware;

    chip = kzalloc(sizeof(*chip), GFP_KERNEL);
    if (!chip) return -ENOMEM;

    // 3. Initialize Data
    chip->substream = substream;    
    chip->period_size = 1024;

    timer_setup(&chip->timer, echo_timer_function, 0);

    runtime->private_data = chip;

    printk(KERN_INFO "EchoDriver: PCM Device Opened\n");
    return 0; 
}

static int my_pcm_close(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip = substream->runtime->private_data;

    if (chip) {
        del_timer_sync(&chip->timer); // Stop the heartbeat
        kfree(chip);                  // Free memory
    }
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