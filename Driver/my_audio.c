#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>    
#include <linux/timer.h>   
#include <linux/jiffies.h> 
#include <linux/mutex.h>
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

/*
 * Shared internal ring buffer for playback -> capture transfer.
 * Playback writes reverb-processed audio here.
 * Capture reads from here and copies into its DMA area.
 *
 * Size is in samples (short), not bytes.
 * 512K samples = ~5.8s of stereo audio @ 44.1kHz
 */
#define ECHO_INTERNAL_BUF_SAMPLES (512 * 1024)

/*
 * Runtime-configurable reverb parameters via /sys/module/my_audio/parameters/
 * Can be changed without reloading the module:
 *   echo 500 | sudo tee /sys/module/my_audio/parameters/reverb_delay_ms
 */
static int reverb_delay_ms = 300;   /* Delay time in ms (10-1500) */
module_param(reverb_delay_ms, int, 0644);
MODULE_PARM_DESC(reverb_delay_ms, "Reverb delay time in milliseconds (10-1500)");

static int reverb_decay = 50;       /* Decay percentage 0-95 */
module_param(reverb_decay, int, 0644);
MODULE_PARM_DESC(reverb_decay, "Reverb decay/feedback percentage (0-95)");

static int reverb_wet = 100;        /* Wet mix percentage 0-100 */
module_param(reverb_wet, int, 0644);
MODULE_PARM_DESC(reverb_wet, "Wet/dry mix percentage (0=dry only, 100=full reverb)");

static short *echo_internal_buf;
static atomic_long_t echo_write_pos = ATOMIC_LONG_INIT(0);
static atomic_long_t echo_read_pos = ATOMIC_LONG_INIT(0);


struct echo_runtime {
    struct snd_pcm_substream *substream; // Reference to the stream
    struct timer_list timer;             // The Heartbeat
    unsigned long hw_ptr;                // Current Position (in frames)
    int is_playback;                     // 1 = playback, 0 = capture
};

static const struct snd_pcm_hardware my_pcm_hardware = {
    .info = (SNDRV_PCM_INFO_MMAP |
             SNDRV_PCM_INFO_MMAP_VALID |
             SNDRV_PCM_INFO_INTERLEAVED |
             SNDRV_PCM_INFO_BLOCK_TRANSFER),
    
    .formats = SNDRV_PCM_FMTBIT_S16_LE,      // 16-bit
    .rates = (SNDRV_PCM_RATE_8000  |          // All standard rates
              SNDRV_PCM_RATE_16000 |
              SNDRV_PCM_RATE_22050 |
              SNDRV_PCM_RATE_32000 |
              SNDRV_PCM_RATE_44100 |
              SNDRV_PCM_RATE_48000 |
              SNDRV_PCM_RATE_96000 |
              SNDRV_PCM_RATE_192000),
    .rate_min = 8000,
    .rate_max = 192000,
    .channels_min = 1,                       // Mono or Stereo
    .channels_max = 2,
    .buffer_bytes_max = 128 * 1024,          // Max Buffer (128KB for higher rates)
    .period_bytes_min = 1024,                // Min Chunk Size
    .period_bytes_max = 64 * 1024,           // Max Chunk Size
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


/*
 * PLAYBACK PROCESSING:
 *   - Reads audio from the playback DMA buffer (where the app wrote)
 *   - Applies configurable reverb using module parameters
 *   - Formula: output = dry_part + wet_part
 *     where wet_part = input + (decay/128) * delayed_sample
 *   - Writes processed samples into the shared internal ring buffer
 *
 * All math uses integer fixed-point (scale factor 128) to avoid
 * floating-point in kernel context.
 */
static void process_playback_chunk(struct echo_runtime *chip)
{
    struct snd_pcm_runtime *runtime;
    short *pb_buf;
    unsigned long pb_buf_samples;
    int frames_per_tick, samples_per_tick;
    int i;
    unsigned long read_idx, delayed_idx;
    short input_sample, delayed_sample;
    int mixed, dry_part, wet_part;

    /* Snapshot the module params (they can change at any time via sysfs) */
    int delay_ms = clamp(reverb_delay_ms, 10, 1500);
    int decay_pct = clamp(reverb_decay, 0, 95);
    int wet_pct = clamp(reverb_wet, 0, 100);

    /* Convert to fixed-point scale of 128 */
    int decay_fp = (decay_pct * 128) / 100;  /* 0..121 */
    int wet_fp   = (wet_pct * 128) / 100;    /* 0..128 */
    int dry_fp   = 128 - wet_fp;             /* 0..128 */

    unsigned long delay_samples;

    /* --- Guard: validate all pointers before touching memory --- */
    if (!chip || !chip->substream) {
        printk(KERN_ERR "EchoDriver: playback chunk - NULL chip or substream\n");
        return;
    }

    runtime = chip->substream->runtime;
    if (!runtime) {
        printk(KERN_ERR "EchoDriver: playback chunk - NULL runtime\n");
        return;
    }

    pb_buf = (short *)runtime->dma_area;
    if (!pb_buf) {
        printk(KERN_ERR "EchoDriver: playback chunk - NULL dma_area\n");
        return;
    }

    if (!echo_internal_buf) {
        printk(KERN_ERR "EchoDriver: playback chunk - NULL internal buffer\n");
        return;
    }

    /* --- Guard: validate runtime parameters --- */
    if (runtime->buffer_size == 0 || runtime->channels == 0 || runtime->rate == 0) {
        printk(KERN_ERR "EchoDriver: playback chunk - invalid runtime params "
               "(buf_size=%lu, ch=%u, rate=%u)\n",
               runtime->buffer_size, runtime->channels, runtime->rate);
        return;
    }

    /* Compute delay in samples: delay_ms * samplerate * channels / 1000 */
    delay_samples = ((unsigned long)delay_ms * runtime->rate * runtime->channels) / 1000;
    if (delay_samples >= ECHO_INTERNAL_BUF_SAMPLES)
        delay_samples = ECHO_INTERNAL_BUF_SAMPLES - 1;

    pb_buf_samples = runtime->buffer_size * runtime->channels;
    frames_per_tick = runtime->rate / 100;
    samples_per_tick = frames_per_tick * runtime->channels;

    for (i = 0; i < samples_per_tick; i++) {
        /* Where in the playback DMA buffer are we reading from? */
        read_idx = ((chip->hw_ptr * runtime->channels) + i) % pb_buf_samples;
        input_sample = pb_buf[read_idx];

        /* Read the delayed sample from the internal buffer for the reverb tail */
        delayed_idx = (atomic_long_read(&echo_write_pos) + ECHO_INTERNAL_BUF_SAMPLES - delay_samples)
                      & (ECHO_INTERNAL_BUF_SAMPLES - 1);
        delayed_sample = echo_internal_buf[delayed_idx];

        /*
         * Apply configurable reverb:
         *   reverb_signal = input + (decay/128) * delayed
         *   output = (dry/128) * input + (wet/128) * reverb_signal
         *
         * All divisions by 128 are done with >>7 for speed.
         */
        wet_part = (int)input_sample + (((int)delayed_sample * decay_fp) >> 7);
        dry_part = ((int)input_sample * dry_fp) >> 7;
        mixed = dry_part + ((wet_part * wet_fp) >> 7);

        /* Clamp to 16-bit range */
        if (mixed > 32767)  mixed = 32767;
        if (mixed < -32768) mixed = -32768;

        /* Write processed sample into shared internal buffer */
        echo_internal_buf[atomic_long_read(&echo_write_pos) & (ECHO_INTERNAL_BUF_SAMPLES - 1)] = (short)mixed;
        atomic_long_inc(&echo_write_pos); // Absolute increment
    }
}


/*
 * CAPTURE PROCESSING:
 *   - Reads processed audio from the shared internal buffer
 *   - Copies it into the capture DMA buffer (where the app reads from)
 */
static void process_capture_chunk(struct echo_runtime *chip)
{
    struct snd_pcm_runtime *runtime;
    short *cap_buf;
    unsigned long cap_buf_samples;
    int frames_per_tick, samples_per_tick;
    int i;
    unsigned long write_idx;

    /* --- Guard: validate all pointers before touching memory --- */
    if (!chip || !chip->substream) {
        printk(KERN_ERR "EchoDriver: capture chunk - NULL chip or substream\n");
        return;
    }

    runtime = chip->substream->runtime;
    if (!runtime) {
        printk(KERN_ERR "EchoDriver: capture chunk - NULL runtime\n");
        return;
    }

    cap_buf = (short *)runtime->dma_area;
    if (!cap_buf) {
        printk(KERN_ERR "EchoDriver: capture chunk - NULL dma_area\n");
        return;
    }

    if (!echo_internal_buf) {
        printk(KERN_ERR "EchoDriver: capture chunk - NULL internal buffer\n");
        return;
    }

    /* --- Guard: validate runtime parameters --- */
    if (runtime->buffer_size == 0 || runtime->channels == 0 || runtime->rate == 0) {
        printk(KERN_ERR "EchoDriver: capture chunk - invalid runtime params "
               "(buf_size=%lu, ch=%u, rate=%u)\n",
               runtime->buffer_size, runtime->channels, runtime->rate);
        return;
    }

    cap_buf_samples = runtime->buffer_size * runtime->channels;
    frames_per_tick = runtime->rate / 100;
    samples_per_tick = frames_per_tick * runtime->channels;

    for (i = 0; i < samples_per_tick; i++) {
        write_idx = ((chip->hw_ptr * runtime->channels) + i) % cap_buf_samples;
        cap_buf[write_idx] = echo_internal_buf[atomic_long_read(&echo_read_pos) & (ECHO_INTERNAL_BUF_SAMPLES - 1)];
        atomic_long_inc(&echo_read_pos); 
    }
}


static void echo_timer_function(struct timer_list *t)
{
    struct echo_runtime *chip;
    struct snd_pcm_substream *substream;
    struct snd_pcm_runtime *runtime;
    int frames_to_process;

    /* --- Guard: recover chip and validate entire pointer chain --- */
    chip = from_timer(chip, t, timer);
    if (!chip) {
        printk(KERN_ERR "EchoDriver: timer - NULL chip, stopping\n");
        return; /* Don't reschedule — timer dies here */
    }

    substream = chip->substream;
    if (!substream) {
        printk(KERN_ERR "EchoDriver: timer - NULL substream, stopping\n");
        return;
    }

    runtime = substream->runtime;
    if (!runtime) {
        printk(KERN_ERR "EchoDriver: timer - NULL runtime, stopping\n");
        return;
    }

    if (runtime->rate == 0) {
        printk(KERN_ERR "EchoDriver: timer - rate is 0, stopping\n");
        return;
    }

    /*
     * Process rate/100 frames per 5ms tick.
     * Since the timer fires at 200Hz (5ms) but we process rate/100 frames
     * per tick, we effectively advance at ~2x real-time speed.
     * This cuts processing time roughly in half.
     */
    frames_to_process = runtime->rate / 100;

    /* Process the appropriate direction */
    if (chip->is_playback) {
        int samples_to_process = frames_to_process * runtime->channels;
        unsigned long write_pos = atomic_long_read(&echo_write_pos);
        unsigned long read_pos = atomic_long_read(&echo_read_pos);
        unsigned long filled = write_pos - read_pos;
        unsigned long space_available = ECHO_INTERNAL_BUF_SAMPLES - filled;
        
        // BUFFER FULL: Jab writer ka buffer space kam ho, toh rukna chahiye
        if (space_available < samples_to_process) {
            // hw_ptr update nahi hoga -> Application (writer) block ho jayegi
            mod_timer(&chip->timer, jiffies + msecs_to_jiffies(5));
            return;
        }
        process_playback_chunk(chip);
    } else {
        int samples_to_process = frames_to_process * runtime->channels;
        unsigned long write_pos = atomic_long_read(&echo_write_pos);
        unsigned long read_pos = atomic_long_read(&echo_read_pos);
        unsigned long filled = write_pos - read_pos;

        // BUFFER EMPTY: Jab padhne ke liye data hi na ho
        if (filled < samples_to_process) {
            // hw_ptr update nahi hoga -> Capture app wait karegi
            mod_timer(&chip->timer, jiffies + msecs_to_jiffies(5));
            return;
        }
        process_capture_chunk(chip);
    }

    /* Move pointer forward with safe wrapping */
    chip->hw_ptr += frames_to_process;
    if (runtime->buffer_size > 0 && chip->hw_ptr >= runtime->buffer_size)
        chip->hw_ptr -= runtime->buffer_size;

    /* Notify ALSA that a period has elapsed */
    snd_pcm_period_elapsed(substream);

    /* Restart timer (5ms = 200Hz tick rate for ~2x speed) */
    mod_timer(&chip->timer, jiffies + msecs_to_jiffies(5));
}

static snd_pcm_uframes_t my_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip;

    if (!substream || !substream->runtime || !substream->runtime->private_data) {
        printk(KERN_ERR "EchoDriver: pointer - NULL substream/runtime/chip\n");
        return 0;
    }

    chip = substream->runtime->private_data;
    return chip->hw_ptr;
}

// The Trigger Callback
static int my_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct echo_runtime *chip;

    if (!substream || !substream->runtime || !substream->runtime->private_data) {
        printk(KERN_ERR "EchoDriver: trigger - NULL substream/runtime/chip\n");
        return -EFAULT;
    }

    chip = substream->runtime->private_data;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        printk(KERN_INFO "EchoDriver: trigger START (%s)\n",
               chip->is_playback ? "playback" : "capture");
        mod_timer(&chip->timer, jiffies + msecs_to_jiffies(10));
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        printk(KERN_INFO "EchoDriver: trigger STOP (%s)\n",
               chip->is_playback ? "playback" : "capture");
        del_timer(&chip->timer);
        break;
    default:
        printk(KERN_WARNING "EchoDriver: trigger - unknown cmd %d\n", cmd);
        return -EINVAL;
    }
    return 0;
}

static int my_pcm_open(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip;
    struct snd_pcm_runtime *runtime;

    if (!substream) {
        printk(KERN_ERR "EchoDriver: open - NULL substream\n");
        return -EINVAL;
    }

    runtime = substream->runtime;
    if (!runtime) {
        printk(KERN_ERR "EchoDriver: open - NULL runtime\n");
        return -EINVAL;
    }

    /* Check that the shared internal buffer exists */
    if (!echo_internal_buf) {
        printk(KERN_ERR "EchoDriver: open - internal buffer not allocated!\n");
        return -ENOMEM;
    }

    runtime->hw = my_pcm_hardware;

    chip = kzalloc(sizeof(*chip), GFP_ATOMIC);
    if (!chip) {
        printk(KERN_ERR "EchoDriver: open - failed to allocate echo_runtime\n");
        return -ENOMEM;
    }

    /* Initialize Data */
    chip->substream = substream;    
    chip->hw_ptr = 0;
    chip->is_playback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);

    timer_setup(&chip->timer, echo_timer_function, 0);

    runtime->private_data = chip;

    printk(KERN_INFO "EchoDriver: PCM %s Opened\n",
           chip->is_playback ? "Playback" : "Capture");
    return 0; 
}

static int my_pcm_close(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip;

    if (!substream || !substream->runtime) {
        printk(KERN_WARNING "EchoDriver: close - NULL substream/runtime\n");
        return 0; /* Nothing to clean up */
    }

    chip = substream->runtime->private_data;

    if (chip) {
        printk(KERN_INFO "EchoDriver: Closing PCM %s\n",
               chip->is_playback ? "Playback" : "Capture");
        del_timer_sync(&chip->timer); /* Stop the heartbeat */
        kfree(chip);                  /* Free memory */
        substream->runtime->private_data = NULL; /* Prevent dangling pointer */
    } else {
        printk(KERN_WARNING "EchoDriver: close - chip was already NULL\n");
    }

    return 0;
}

// HW PARAMS: "I accept these settings"
static int my_pcm_hw_params(struct snd_pcm_substream *substream,
                            struct snd_pcm_hw_params *params)
{
    return 0;
}

// PREPARE: "I am ready to start"
static int my_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct echo_runtime *chip;

    if (!substream || !substream->runtime || !substream->runtime->private_data) {
        printk(KERN_ERR "EchoDriver: prepare - NULL substream/runtime/chip\n");
        return -EFAULT;
    }

    chip = substream->runtime->private_data;
    chip->hw_ptr = 0;

    /* When playback prepares, reset the shared buffer so capture stays in sync */
    if (chip->is_playback && echo_internal_buf) {
        memset(echo_internal_buf, 0, ECHO_INTERNAL_BUF_SAMPLES * sizeof(short));
        atomic_long_set(&echo_write_pos, 0);
        atomic_long_set(&echo_read_pos, 0);
        printk(KERN_INFO "EchoDriver: prepare playback - reset internal buffer\n");
    }

    printk(KERN_INFO "EchoDriver: prepare %s (rate=%u, channels=%u, buf_size=%lu)\n",
           chip->is_playback ? "playback" : "capture",
           substream->runtime->rate,
           substream->runtime->channels,
           substream->runtime->buffer_size);
    return 0;
}



static const struct snd_pcm_ops my_reverb_ops  = {

    .open = my_pcm_open,
    .close = my_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params =  my_pcm_hw_params,
    .prepare =    my_pcm_prepare,
    .trigger =    my_pcm_trigger,
    .pointer =    my_pcm_pointer,
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

    /* Allocate the shared internal buffer */
    echo_internal_buf = vmalloc(ECHO_INTERNAL_BUF_SAMPLES * sizeof(short));
    if (!echo_internal_buf) {
        printk(KERN_ERR "EchoDriver: Failed to allocate internal buffer\n");
        return -ENOMEM;
    }
    atomic_long_set(&echo_write_pos, 0);
    atomic_long_set(&echo_read_pos, 0);

    err = echo_register_platform_device();
    if (err)
        goto err_buf;

    err = echo_create_card();
    if (err)
        goto err_pdev;

    err = echo_create_pcm();
    if (err)
        goto err_card;

    err = echo_register_card();
    if (err)
        goto err_card;

    printk(KERN_INFO "EchoDriver: Loaded successfully!\n");
    return 0;

    err_card:
        snd_card_free(my_reverb_card);
    err_pdev:
        platform_device_unregister(my_pdev);
    err_buf:
        vfree(echo_internal_buf);
        echo_internal_buf = NULL;
    
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
    if (echo_internal_buf) {
        vfree(echo_internal_buf);
        echo_internal_buf = NULL;
    }
    atomic_long_set(&echo_write_pos, 0);
    atomic_long_set(&echo_read_pos, 0);
    printk(KERN_INFO "EchoDriver: Unloaded.\n");
}

module_init(echo_probe); 
module_exit(echo_exit);