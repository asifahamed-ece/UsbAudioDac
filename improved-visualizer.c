/* Enhanced Audio Visualizer with Synthwave Palette
 * Improvements:
 * 1. Smooth color gradients (cyan→purple→orange)
 * 2. 2-3 pixel peak dots (reduced graininess)
 * 3. Particle system (8 particles)
 * 4. Loading animations
 * 5. Smooth transitions
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

/* ST7735 128x128 display constants */
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  128

/* Synthwave RGB565 Color Palette */
#define COL_BG          0x0000      /* black */
#define COL_CYAN        0x077F      /* bright cyan (#58a6ff) */
#define COL_PURPLE      0xA0DE      /* purple (#a371f7) */
#define COL_ORANGE      0xFD0F      /* orange (#ffae57) */
#define COL_LIGHT_GRAY  0x8410      /* light gray */
#define COL_DARK_GRAY   0x2945      /* dark gray */
#define COL_WHITE       0xFFFF      /* white */

/* Spectrum Analyzer Layout */
#define BARS_TOP         20         /* first pixel row of bar area */
#define BARS_BOTTOM      110        /* bottom pixel row (max bar height) */
#define BARS_MAX_HEIGHT  90         /* maximum bar height in pixels */
#define BAR_WIDTH        6          /* width of each bar */
#define BAR_GAP          1          /* gap between bars */
#define NUM_BANDS        16         /* number of frequency bands */
#define MARGIN_X         8          /* left margin */

/* Animation States */
#define LOADING_SPLASH   0
#define LOADING_PROGRESS 1
#define LOADING_MAIN     2

/* Particle System */
#define MAX_PARTICLES   8
#define PARTICLE_LIFE   30          /* frames until particle fades */

/* Simulated display functions (for testing) */
void fill_rect(int x, int y, int w, int h, uint16_t color) {
    printf("Fill rect: [%d,%d] %dx%d color: 0x%04X\n", x, y, w, h, color);
}

void draw_string(int x, int y, const char* text, uint16_t color) {
    printf("Draw text: '%s' at [%d,%d] color: 0x%04X\n", text, x, y, color);
}

void clear_screen() {
    printf("Clear screen\n");
}

/* Particle Structure */
typedef struct {
    int x, y;               /* position */
    int vx, vy;             /* velocity */
    int life;               /* remaining life */
    uint16_t color;         /* particle color */
    int frequency_band;     /* associated frequency band */
} Particle;

/* Visualizer State */
static uint8_t bar_heights[NUM_BANDS];
static uint8_t peak_heights[NUM_BANDS];
static uint8_t peak_hold[NUM_BANDS];
static uint8_t rendered_heights[NUM_BANDS];
static Particle particles[MAX_PARTICLES];
static int loading_state = LOADING_SPLASH;
static int spinner_phase = 0;
static int progress_percent = 0;
static uint32_t frame_count = 0;

/* Smooth color interpolation between synthwave colors */
uint16_t interpolate_color(uint16_t color1, uint16_t color2, float t) {
    /* Extract RGB565 components */
    int r1 = (color1 >> 11) & 0x1F;
    int g1 = (color1 >> 5) & 0x3F;
    int b1 = color1 & 0x1F;

    int r2 = (color2 >> 11) & 0x1F;
    int g2 = (color2 >> 5) & 0x3F;
    int b2 = color2 & 0x1F;

    /* Linear interpolation */
    int r = (int)(r1 + (r2 - r1) * t);
    int g = (int)(g1 + (g2 - g1) * t);
    int b = (int)(b1 + (b2 - b1) * t);

    /* Clamp values */
    if (r < 0) r = 0; if (r > 31) r = 31;
    if (g < 0) g = 0; if (g > 63) g = 63;
    if (b < 0) b = 0; if (b > 31) b = 31;

    return (r << 11) | (g << 5) | b;
}

/* Get bar color based on height (smooth gradient) */
uint16_t get_bar_color(int height) {
    float normalized_height = (float)height / BARS_MAX_HEIGHT;

    if (normalized_height < 0.33f) {
        /* Cyan zone (top 33%) */
        return COL_CYAN;
    } else if (normalized_height < 0.66f) {
        /* Purple zone (middle 33%) */
        float t = (normalized_height - 0.33f) / 0.33f;
        return interpolate_color(COL_CYAN, COL_PURPLE, t);
    } else {
        /* Orange zone (bottom 33%) */
        float t = (normalized_height - 0.66f) / 0.34f;
        return interpolate_color(COL_PURPLE, COL_ORANGE, t);
    }
}

/* Draw a bar with smooth gradient */
void draw_bar(int bar_index, int height) {
    int x = MARGIN_X + bar_index * (BAR_WIDTH + BAR_GAP);
    int y_bottom = BARS_BOTTOM;
    int y_top = y_bottom - height;

    /* Fill entire bar area */
    fill_rect(x, y_top, BAR_WIDTH, height, get_bar_color(height));
}

/* Draw peak dot (2-3 pixels for reduced graininess) */
void draw_peak_dot(int bar_index, int height) {
    int x = MARGIN_X + bar_index * (BAR_WIDTH + BAR_GAP);
    int y = BARS_BOTTOM - height;

    /* Draw a 2x2 dot instead of 1x1 for smoother appearance */
    fill_rect(x, y - 1, BAR_WIDTH, 2, COL_WHITE);
}

/* Update peak hold system with smooth decay */
void update_peak(int bar_index, int current_height) {
    static int peak_counter[NUM_BANDS] = {0};

    /* Update peak if new height is higher */
    if (current_height > peak_heights[bar_index]) {
        peak_heights[bar_index] = current_height;
        peak_counter[bar_index] = 15;  /* Hold for 15 frames */
    }

    /* Smooth decay when not holding */
    if (peak_counter[bar_index] == 0 && peak_heights[bar_index] > current_height) {
        /* Exponential decay: reduces height by ~5% each frame */
        peak_heights[bar_index] = (uint8_t)(peak_heights[bar_index] * 0.95f);

        /* Minimum decay of 1 pixel per frame */
        if (peak_heights[bar_index] > current_height + 1) {
            peak_heights[bar_index]--;
        }
    } else if (peak_counter[bar_index] > 0) {
        peak_counter[bar_index]--;
    }
}

/* Initialize particle system */
void init_particles() {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particles[i].life = 0;  /* Start with all particles inactive */
    }
}

/* Spawn a new particle */
void spawn_particle(int band_index, int band_height) {
    /* Find an inactive particle */
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life == 0) {
            /* Position particle at the top of the bar */
            int x = MARGIN_X + band_index * (BAR_WIDTH + BAR_GAP) + BAR_WIDTH/2;
            int y = BARS_BOTTOM - band_height;

            particles[i].x = x;
            particles[i].y = y;

            /* Random velocity based on band activity */
            particles[i].vx = (rand() % 3) - 1;  /* -1, 0, or 1 */
            particles[i].vy = -(rand() % 2 + 1); /* Move upward */

            particles[i].life = PARTICLE_LIFE;
            particles[i].color = get_bar_color(band_height);
            particles[i].frequency_band = band_index;

            break;
        }
    }
}

/* Update and draw particles */
void update_particles() {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life > 0) {
            /* Update position */
            particles[i].x += particles[i].vx;
            particles[i].y += particles[i].vy;

            /* Apply gravity (slightly downward over time) */
            particles[i].vy += 1;

            /* Fade particle */
            particles[i].life--;

            /* Draw particle */
            int size = 1 + (particles[i].life / 10);  /* Larger when newer */
            fill_rect(particles[i].x, particles[i].y, size, size, particles[i].color);

            /* Draw trail effect */
            if (particles[i].life > 20) {
                fill_rect(particles[i].x, particles[i].y + 1, 1, 1, COL_WHITE);
            }
        }
    }
}

/* Loading splash animation */
void show_loading_splash() {
    clear_screen();

    /* Title */
    draw_string(15, 40, "USB AUDIO DAC", COL_CYAN);
    draw_string(30, 50, "Synthwave Edition", COL_PURPLE);

    /* Animated particles around title */
    for (int i = 0; i < 12; i++) {
        int angle = i * 30;
        int radius = 20;
        int x = DISPLAY_WIDTH/2 + (int)(cos(angle * 3.14159 / 180.0) * radius);
        int y = DISPLAY_HEIGHT/2 + (int)(sin(angle * 3.14159 / 180.0) * radius);

        /* Cycle through colors */
        uint16_t color;
        if (i < 4) color = COL_CYAN;
        else if (i < 8) color = COL_PURPLE;
        else color = COL_ORANGE;

        fill_rect(x, y, 2, 2, color);
    }

    printf("Loading splash complete\n");
}

/* Loading progress bar with spinner */
void show_loading_progress() {
    int bar_start_x = 20;
    int bar_start_y = 80;
    int bar_width = 88;
    int bar_height = 8;

    /* Progress bar outline */
    fill_rect(bar_start_x, bar_start_y, bar_width, 1, COL_DARK_GRAY);
    fill_rect(bar_start_x, bar_start_y + bar_height, bar_width, 1, COL_DARK_GRAY);
    fill_rect(bar_start_x, bar_start_y, 1, bar_height, COL_DARK_GRAY);
    fill_rect(bar_start_x + bar_width, bar_start_y, 1, bar_height, COL_DARK_GRAY);

    /* Animate progress */
    for (progress_percent = 0; progress_percent <= 100; progress_percent += 5) {
        int progress_width = (progress_percent * bar_width) / 100;

        /* Fill progress area */
        fill_rect(bar_start_x + 1, bar_start_y + 1, progress_width - 2, bar_height - 2, COL_CYAN);

        /* Spinner position */
        int spinner_x = bar_start_x + bar_width + 10;
        int spinner_y = bar_start_y + bar_height / 2;
        spinner_phase = (progress_percent * 360) / 100;

        /* Draw spinner arm */
        int arm_length = 6;
        int arm_x = spinner_x + (int)(cos(spinner_phase * 3.14159 / 180.0) * arm_length);
        int arm_y = spinner_y + (int)(sin(spinner_phase * 3.14159 / 180.0) * arm_length);

        fill_rect(arm_x, arm_y, 2, 2, COL_PURPLE);

        /* Clear spinner arm for next frame */
        fill_rect(arm_x, arm_y, 2, 2, COL_BG);
    }

    printf("Loading progress complete\n");
}

/* Initialize visualizer */
void visualizer_init() {
    clear_screen();

    /* Show loading animations */
    show_loading_splash();

    /* Clear for main display */
    clear_screen();

    /* Draw static elements */
    draw_string(2, 5, "USB AUDIO", COL_CYAN);
    draw_string(96, 5, "44.1k", COL_PURPLE);

    /* Draw baseline */
    fill_rect(0, BARS_BOTTOM, DISPLAY_WIDTH, 1, COL_DARK_GRAY);

    /* Frequency labels */
    draw_string(5, DISPLAY_HEIGHT - 8, "60", COL_LIGHT_GRAY);
    draw_string(47, DISPLAY_HEIGHT - 8, "1k", COL_LIGHT_GRAY);
    draw_string(107, DISPLAY_HEIGHT - 8, "16k", COL_LIGHT_GRAY);

    /* Initialize particle system */
    init_particles();

    /* Zero all state */
    for (int i = 0; i < NUM_BANDS; i++) {
        bar_heights[i] = 0;
        peak_heights[i] = 0;
        peak_hold[i] = 0;
        rendered_heights[i] = 0;
    }

    printf("Visualizer initialized\n");
}

/* Main update function - called every frame */
void visualizer_update(int frame_num) {
    /* Simulate audio input - generate random bar heights */
    for (int i = 0; i < NUM_BANDS; i++) {
        int base_height = 30 + (int)(sin(frame_num * 0.1 + i * 0.5) * 20);
        int random_variation = rand() % 20;

        bar_heights[i] = base_height + random_variation;
        if (bar_heights[i] > BARS_MAX_HEIGHT) bar_heights[i] = BARS_MAX_HEIGHT;
    }

    /* Process each band */
    for (int i = 0; i < NUM_BANDS; i++) {
        int current_height = bar_heights[i];

        /* Update peak with smooth decay */
        update_peak(i, current_height);

        /* Draw bar with smooth color gradient */
        draw_bar(i, current_height);

        /* Draw peak dot (reduced graininess) */
        if (peak_heights[i] > 0) {
            draw_peak_dot(i, peak_heights[i]);
        }

        /* Occasionally spawn particles */
        if ((rand() % 100) < 20 && current_height > 40) {  /* 20% chance per frame if bar is tall */
            spawn_particle(i, current_height);
        }
    }

    /* Update and draw particles */
    update_particles();

    /* Draw peak hold indicator */
    draw_string(DISPLAY_WIDTH - 30, DISPLAY_HEIGHT - 8, "PEAK", COL_LIGHT_GRAY);

    /* Increment frame counter */
    frame_count++;

    if (frame_count % 30 == 0) {
        printf("Frame %d: Processing %d bands\n", frame_count, NUM_BANDS);
    }
}

/* Main test function */
int main() {
    printf("Enhanced Audio Visualizer - Synthwave Edition\n");
    printf("Display: %dx%d, %d frequency bands\n\n", DISPLAY_WIDTH, DISPLAY_HEIGHT, NUM_BANDS);

    /* Initialize */
    visualizer_init();

    /* Simulate 100 frames of animation */
    for (int frame = 0; frame < 100; frame++) {
        printf("\n--- Frame %d ---\n", frame);
        visualizer_update(frame);
    }

    printf("\nVisualizer test complete\n");
    return 0;
}