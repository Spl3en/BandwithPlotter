#include <curl/curl.h>
#include <SFML/Graphics.h>
#include "utils/utils.h"
#include "dbg/dbg.h"
#include "BbQueue/BbQueue.h"

// Update tick frequency
#define UPDATE_TICK_FREQUENCY 0.01

// Size in pixels between each tick on X axis
#define X_TILE_SIZE 150

/** === Type declaration === */
typedef struct {
    float width, height; // screen size

    // Axis
    sfRectangleShape *axis[2];
    sfVector2f padding; // Axis padding
    sfVector2f axisSize; // Axis size
    double startAxisTime;

    // Progress averageBandwith
    sfVertexArray *averageBandwith;
    sfVertexArray *averageBandwithData;
    sfVertexArray *currentBandwith;
    sfVertexArray *currentBandwithData;
    sfText *avgBandwidthText;
    sfText *currentBandwithText;

    // Download information
    sfText *timeText;
    sfText *sizeText;
    sfText *urlText;
    sfText *maxSpeedText;
    sfText *legendAvg;
    sfText *legendCur;
    sfVertexArray *legendAvgColor;
    sfVertexArray *legendCurColor;

}   Graphics;

typedef struct {
    // Application data
    CURL *curl;
    sfRenderWindow *window;
    Graphics graphics;

    // SFML <-> CURL communication
    sfMutex *mutex;
    BbQueue *dataQueue;

    // Destination file
    FILE *output;
} Application;

typedef struct {
    double time;
    double speed;
    double size;
    double lastSecondSpeed;
} VertexData;

/** === Prototypes === */
// Initialize SFML window
bool init_sfml (sfRenderWindow **_window);

// Initialize CURL library
bool init_curl (CURL **_curl, char *url);

// CURL progress callback
int progress_callback (Application *self, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

// CURL write callback
size_t write_callback (void *buf, size_t size, size_t nmemb, Application *p);

// Draw in SFML window
void render (Application *self);

// Get SFML inputs
bool input (Application *self);

// Update the application state
void update (Application *self);

/** === Implementation === */
bool init_sfml (sfRenderWindow **_window) {

	sfVideoMode desktop = sfVideoMode_getDesktopMode ();

    sfRenderWindow *window = sfRenderWindow_create (
        (sfVideoMode) {
            // 2/3 of screen space
            .width  = desktop.width * 0.666,
            .height = desktop.height * 0.333,
            .bitsPerPixel = 32
        },
        "Bandwith Plotter",
        sfDefaultStyle,
        (sfContextSettings []) {{
            .depthBits = 24,
            .stencilBits = 8,
            .antialiasingLevel = 0,
            .majorVersion = 2,
            .minorVersion = 1,
        }}
    );

    if (!window) {
        printf ("Cannot create rendering window.");
        return false;
    }

    // sfRenderWindow_setVerticalSyncEnabled (window, true);

    *_window = window;

    return true;
}

bool init_curl (CURL **_curl, char *url) {

    CURL *curl = curl_easy_init ();

    curl_easy_setopt (curl, CURLOPT_URL, url);
    curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0);

    *_curl = curl;
    return true;
}

void get_vertex_x (sfVertex *v, double time, double start) {
    v->position.x = (time - start) * X_TILE_SIZE;
}

void get_vertex_y (sfVertex *v, double axisSizeY, double speed, double limitSpeed) {
    v->position.y = axisSizeY - (speed * axisSizeY / limitSpeed);
}

// Function helper for computing the new vertex position
void get_vertex_pos (sfVertex *v, sfVector2f axisSize, double startAxisTime, double time, double speed, double limitSpeed) {
    get_vertex_x(v, time, startAxisTime);
    get_vertex_y(v, axisSize.y, speed, limitSpeed);

    // Check if we overflow the X axis
    if (v->position.x >= axisSize.x) {
        v->position.x = axisSize.x;
    }
}

void update (Application *self) {

    VertexData *data = NULL;
    Graphics *graphics = &self->graphics;

    // Get the queue data if there is a vertex inside it
    sfMutex_lock(self->mutex);
    data = bb_queue_pop(self->dataQueue);
    sfMutex_unlock(self->mutex);

    // There is something in the queue whenever curl ticks
    if (!data) {
        return;
    }

    // Get max Y
    static double limitSpeed = 1000;
    size_t avgSize = sfVertexArray_getVertexCount(graphics->averageBandwith);
    size_t curSize = sfVertexArray_getVertexCount(graphics->currentBandwith);

    // Add padding to a vertex
    void add_padding (sfVertex *v) {
        v->position.x += graphics->padding.x;
        v->position.y += graphics->padding.y;
    }

    // Offset all the vertices to a new position
    void relocate_vertices(double limitSpeed) {
        for (size_t i = 0; i < avgSize; i++) {
            sfVertex *v = sfVertexArray_getVertex(graphics->averageBandwith, i);
            sfVertex *vData = sfVertexArray_getVertex(graphics->averageBandwithData, i);
            get_vertex_pos(v, graphics->axisSize, graphics->startAxisTime, vData->position.x, vData->position.y, limitSpeed);
            add_padding(v);
        }
        for (size_t i = 0; i < curSize; i++) {
            sfVertex *v = sfVertexArray_getVertex(graphics->currentBandwith, i);
            sfVertex *vData = sfVertexArray_getVertex(graphics->currentBandwithData, i);
            get_vertex_pos(v, graphics->axisSize, graphics->startAxisTime, vData->position.x, vData->position.y, limitSpeed);
            add_padding(v);
        }
    }

    // Check if we need to relocate vertices
    if (data->speed >= limitSpeed) {
        limitSpeed = data->speed;
        relocate_vertices(limitSpeed);
    }
    if (data->lastSecondSpeed >= limitSpeed) {
        limitSpeed = data->lastSecondSpeed;
        relocate_vertices(limitSpeed);
    }

    // Get current vertices position
    sfVertex averageBpVx, currentBpVx;
    get_vertex_pos (&averageBpVx, graphics->axisSize, graphics->startAxisTime, data->time, data->speed, limitSpeed);
    get_vertex_pos (&currentBpVx, graphics->axisSize, graphics->startAxisTime, data->time, data->lastSecondSpeed, limitSpeed);

    if (averageBpVx.position.x >= graphics->axisSize.x) {

        // Offset all the previous vertices
        void offset_vertices (
            sfVertexArray **_band,
            sfVertexArray **_bandData,
            sfVertexArray *curB,
            sfVertexArray *curBData
        ) {
            sfVertexArray *band = sfVertexArray_create();
            sfVertexArray *bandData = sfVertexArray_create();
            sfVertexArray_setPrimitiveType(band, sfLinesStrip);

            for (size_t i = 1; i < avgSize; i++) {
                sfVertex *v = sfVertexArray_getVertex(curB, i);
                sfVertex *vData = sfVertexArray_getVertex(curBData, i);
                if (i == 1) {
                    graphics->startAxisTime = vData->position.x;
                }
                sfVertexArray_append(band, *v);
                sfVertexArray_append(bandData, *vData);
            }

            *_band = band;
            *_bandData = bandData;
        }

        sfVertexArray *avgBand = NULL;
        sfVertexArray *avgBandData = NULL;
        sfVertexArray *curBand = NULL;
        sfVertexArray *curBandData = NULL;

        offset_vertices(&avgBand, &avgBandData, graphics->averageBandwith, graphics->averageBandwithData);
        offset_vertices(&curBand, &curBandData, graphics->currentBandwith, graphics->currentBandwithData);

        sfVertexArray_destroy(graphics->averageBandwith);
        sfVertexArray_destroy(graphics->averageBandwithData);
        sfVertexArray_destroy(graphics->currentBandwith);
        sfVertexArray_destroy(graphics->currentBandwithData);

        graphics->averageBandwith = avgBand;
        graphics->averageBandwithData = avgBandData;
        graphics->currentBandwith = curBand;
        graphics->currentBandwithData = curBandData;

        relocate_vertices(limitSpeed);
    }

    // Apply padding
    add_padding(&averageBpVx);
    add_padding(&currentBpVx);

    // Apply color
    averageBpVx.color = sfRed;
    currentBpVx.color = sfYellow;

    sfVertex averageBpVxData = {.position = {.x = data->time, .y = data->speed}};
    sfVertex currentBpVxData = {.position = {.x = data->time, .y = data->lastSecondSpeed}};

    // Add them to the vertices array
    sfVertexArray_append (graphics->averageBandwith, averageBpVx);
    sfVertexArray_append (graphics->averageBandwithData, averageBpVxData);
    sfVertexArray_append (graphics->currentBandwith, currentBpVx);
    sfVertexArray_append (graphics->currentBandwithData, currentBpVxData);

    // Update text string and position
    char string[100];
    sprintf(string, "%.0f KB/s", data->speed);
    sfText_setPosition(graphics->avgBandwidthText, (sfVector2f){
        .x = averageBpVx.position.x + 15,
        .y = averageBpVx.position.y - 15
    });
    sfText_setString(graphics->avgBandwidthText, string);

    sprintf(string, "%.0f KB/s", data->lastSecondSpeed);
    sfText_setPosition(graphics->currentBandwithText, (sfVector2f){
        .x = currentBpVx.position.x + 15,
        .y = currentBpVx.position.y - 15
    });
    sfText_setString(graphics->currentBandwithText, string);

    // Update time text
    sprintf(string, "Time : %.2f seconds", data->time);
    sfText_setString(graphics->timeText, string);

    // Update size text
    sprintf(string, "Size downloaded : %.0f MB", data->size / 1024);
    sfText_setString(graphics->sizeText, string);

    // Update max speed text
    sprintf(string, "%.0f KB/s", limitSpeed);
    sfText_setString(graphics->maxSpeedText, string);
}

int progress_callback (Application *self, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {

    static BbQueue lastSecondQueue = bb_queue_local_decl();
    static BbQueue clearQueue = bb_queue_local_decl();
    static float lastTime = 0.0;

    // Get current time
    double time;
    curl_easy_getinfo(self->curl, CURLINFO_TOTAL_TIME, &time);

    // Get total size
    double size;
    curl_easy_getinfo(self->curl, CURLINFO_SIZE_DOWNLOAD, &size);

    VertexData *data = malloc(sizeof(VertexData));
    data->time = time;
    data->size = size / 1024;
    bb_queue_add(&lastSecondQueue, data);

    // Update every tick
    if (time - lastTime >= UPDATE_TICK_FREQUENCY) {
        lastTime = time;

        // Get download speed
        double speed;
        curl_easy_getinfo(self->curl, CURLINFO_SPEED_DOWNLOAD, &speed);
        data->speed = speed / 1024; // KB/s

        // Get number of bytes during the last second only
        double bytesCount = 0;
        double lastBytesCount = 0;
        foreach_bbqueue_item(&lastSecondQueue, VertexData *data) {
            if (data->time <= time && data->time >= (time - 1)) {
                if (lastBytesCount == 0) {
                    lastBytesCount = data->size;
                }
                bytesCount += (data->size - lastBytesCount);
                lastBytesCount = data->size;
            } else if (time >= (data->time + 20)) {
                // Don't keep more than 20 seconds of cache
                bb_queue_add(&clearQueue, data);
            }
        }
        // Cleanup
        while (bb_queue_get_length(&clearQueue)) {
            VertexData *data = bb_queue_pop(&clearQueue);
            bb_queue_remv(&lastSecondQueue, data);
            free(data);
        }
        data->lastSecondSpeed = bytesCount;

        // Push data to the shared data queue
        sfMutex_lock(self->mutex);
        bb_queue_push(self->dataQueue, data);
        sfMutex_unlock(self->mutex);
    }

    return 0;
}

size_t write_callback (void *buf, size_t size, size_t nmemb, Application *self) {
    if (!self->output) {
        // Don't write anything to disk
        return size * nmemb;
    }

    fwrite(buf, size, nmemb, self->output);
    return size * nmemb;
}

void render (Application *self) {

    sfRenderWindow *window = self->window;
    Graphics *graphics = &self->graphics;

    // Clear
    sfRenderWindow_clear (window, sfBlack);

    // Draw axis
    sfRenderWindow_drawRectangleShape (window, graphics->axis[0], NULL);
    sfRenderWindow_drawRectangleShape (window, graphics->axis[1], NULL);

    // Draw bandwith text and curves
    sfRenderWindow_drawText (window, graphics->avgBandwidthText, NULL);
    sfRenderWindow_drawText (window, graphics->currentBandwithText, NULL);
    sfRenderWindow_drawVertexArray (window, graphics->averageBandwith, NULL);
    sfRenderWindow_drawVertexArray (window, graphics->currentBandwith, NULL);

    // Draw download information
    sfRenderWindow_drawText (window, graphics->timeText, NULL);
    sfRenderWindow_drawText (window, graphics->sizeText, NULL);
    sfRenderWindow_drawText (window, graphics->urlText, NULL);
    sfRenderWindow_drawText (window, graphics->maxSpeedText, NULL);

    // Draw legend
    sfRenderWindow_drawVertexArray (window, graphics->legendAvgColor, NULL);
    sfRenderWindow_drawVertexArray (window, graphics->legendCurColor, NULL);
    sfRenderWindow_drawText (window, graphics->legendAvg, NULL);
    sfRenderWindow_drawText (window, graphics->legendCur, NULL);

    // Render to the window
    sfRenderWindow_display (window);
}

bool input (Application *self) {

    // ESC = Quit
    if (sfKeyboard_isKeyPressed (sfKeyEscape)) {
        sfRenderWindow_close (self->window);
        return true;
    }

    return false;
}

bool init_graphics (Graphics *self, char *url) {

    sfFont *font;

	sfVideoMode desktop = sfVideoMode_getDesktopMode ();
	self->width = desktop.width * 0.666;
	self->height = desktop.height * 0.333;
	self->padding = (sfVector2f) {50, 60};
	self->startAxisTime = 0.0;

    // X Axis
    sfVector2f xAxisPos = {.x = self->padding.x, .y = self->height - self->padding.y};
    self->axisSize.x = self->width - (self->padding.x * 2 + 100);
    sfRectangleShape *xAxis = self->axis[0] = sfRectangleShape_create();
    sfRectangleShape_setPosition (xAxis, xAxisPos);
    sfRectangleShape_setSize (xAxis, (sfVector2f) {.x = self->axisSize.x, .y = 1});
    sfRectangleShape_setFillColor (xAxis, sfWhite);

    // Y axis
    sfVector2f yAxisPos = {.x = self->padding.x, .y = self->padding.y};
    self->axisSize.y = self->height - (self->padding.y * 2);
    sfRectangleShape *yAxis = self->axis[1] = sfRectangleShape_create();
    sfRectangleShape_setPosition (yAxis, yAxisPos);
    sfRectangleShape_setSize (yAxis, (sfVector2f) {.x = 1, .y = self->axisSize.y});
    sfRectangleShape_setFillColor (yAxis, sfWhite);

    // Average bandwith vertex array
    self->averageBandwith = sfVertexArray_create ();
    self->averageBandwithData = sfVertexArray_create ();
    sfVertexArray_setPrimitiveType(self->averageBandwith, sfLinesStrip);

    // Current bandwith vertex array
    self->currentBandwith = sfVertexArray_create ();
    self->currentBandwithData = sfVertexArray_create ();
    sfVertexArray_setPrimitiveType(self->currentBandwith, sfLinesStrip);

    // Font
    if (!(font = sfFont_createFromFile("visitor2.ttf"))) {
        // Find it on Windows Fonts folder
        if (!(font = sfFont_createFromFile("C:/Windows/Fonts/visitor2.ttf"))) {
            error("Cannot find font.");
        }
    }

    // Bandwith text
    self->avgBandwidthText = sfText_create ();
    sfText_setCharacterSize(self->avgBandwidthText, 30);
    sfText_setFont(self->avgBandwidthText, font);
    self->currentBandwithText = sfText_create ();
    sfText_setCharacterSize(self->currentBandwithText, 30);
    sfText_setFont(self->currentBandwithText, font);

    // Total time text
    self->timeText = sfText_create ();
    sfText_setCharacterSize(self->timeText, 20);
    sfText_setFont(self->timeText, font);
    sfText_setPosition(self->timeText, (sfVector2f){
        .x = self->axisSize.x - self->padding.x - 50,
        .y = self->height - self->padding.y});

    // Total size text
    self->sizeText = sfText_create ();
    sfText_setCharacterSize(self->sizeText, 20);
    sfText_setFont(self->sizeText, font);
    sfText_setPosition(self->sizeText, (sfVector2f){.x = self->width / 2 - 100, .y = 0});

    // URL text
    self->urlText = sfText_create ();
    sfText_setCharacterSize(self->urlText, 20);
    sfText_setFont(self->urlText, font);
    sfText_setPosition(self->urlText, (sfVector2f){.x = self->width - 300, .y = 0});
    sfText_setString(self->urlText, url);

    // Max speed text
    self->maxSpeedText = sfText_create ();
    sfText_setCharacterSize(self->maxSpeedText, 20);
    sfText_setFont(self->maxSpeedText, font);
    sfText_setPosition(self->maxSpeedText, (sfVector2f){.x = 10, .y = self->padding.y - 30});
    sfText_setString(self->maxSpeedText, url);

    // Legend
    self->legendAvg = sfText_create ();
    sfText_setCharacterSize(self->legendAvg, 20);
    sfText_setFont(self->legendAvg, font);
    sfText_setPosition (self->legendAvg, (sfVector2f){.x = 50, .y = self->height - 30});
    sfText_setString(self->legendAvg, "Average speed");

    self->legendCur = sfText_create ();
    sfText_setCharacterSize(self->legendCur, 20);
    sfText_setFont(self->legendCur, font);
    sfText_setPosition (self->legendCur, (sfVector2f){.x = 50, .y = self->height - 50});
    sfText_setString(self->legendCur, "Current speed");

    self->legendAvgColor = sfVertexArray_create ();
    self->legendCurColor = sfVertexArray_create ();
    sfVertexArray_setPrimitiveType(self->legendAvgColor, sfLinesStrip);
    sfVertexArray_setPrimitiveType(self->legendCurColor, sfLinesStrip);

    sfVertex avgColor = {.position = {.x = 10, .y = self->height - 15}, .color = sfRed};
    sfVertex curColor = {.position = {.x = 10, .y = self->height - 35}, .color = sfYellow};
    sfVertex avgColor2 = avgColor;
    avgColor2.position.x += 30;
    sfVertex curColor2 = curColor;
    curColor2.position.x += 30;

    sfVertexArray_append (self->legendAvgColor, avgColor);
    sfVertexArray_append (self->legendAvgColor, avgColor2);
    sfVertexArray_append (self->legendCurColor, curColor);
    sfVertexArray_append (self->legendCurColor, curColor2);

    return true;
}

bool application_init (Application *self, char *url, char *filename) {

    memset(self, 0, sizeof(*self));

    // Initialize SFML
    if (!(init_sfml (&self->window))) {
        error ("Cannot initialize window.");
        return false;
    }

    // Initialize CURL
    if (!(init_curl (&self->curl, url))) {
        error ("Cannot initialize window.");
        return false;
    }

    // Initialize graphics
    if (!(init_graphics (&self->graphics, url))) {
        error ("Cannot initialize graphics.");
        return false;
    }

    if (filename) {
        if (!(self->output = fopen(filename, "w+"))) {
            error("Cannot open '%s'.");
            return false;
        }
    }

    self->mutex = sfMutex_create ();
    self->dataQueue = bb_queue_new ();

    // Attach Application data to CURL callback
    curl_easy_setopt (self->curl, CURLOPT_XFERINFODATA, self);
    curl_easy_setopt (self->curl, CURLOPT_WRITEDATA, self);

    return true;
}

void start_download (void *_self) {
    Application *self = _self;
    curl_easy_perform (self->curl);
    fclose(self->output);
}

void application_run (Application *self) {

    // Start downloading
    sfThread *curlThread = sfThread_create (start_download, self);
    sfThread_launch (curlThread);

    // Main loop
    while (sfRenderWindow_isOpen(self->window)) {

        // Process events
        sfEvent event;
        while (sfRenderWindow_pollEvent(self->window, &event)) {
            if (event.type == sfEvtClosed) {
                sfRenderWindow_close (self->window);
            }
        }

        // Process inputs
        input (self);
        // Update graphics
        update (self);
        // Render to window
        render (self);

        Sleep(1);
    }
}

int main (int argc, char **argv)
{
    // === Process parameters ===
    char *url = (argc >= 2) ? argv[1] : "test-debit.free.fr/image.iso";
    char *filename = (argc >= 3) ? argv[2] : NULL;

    info("Usage : BandwithPlotter <url> <output filename>", argv[0]);

    // === Initialize and run the application ===
    Application appInfo;
    if (!(application_init (&appInfo, url, filename))) {
        error("Cannot initialize application correctly.");
        return -1;
    }

    application_run (&appInfo);

    // Cleanup
    sfRenderWindow_destroy (appInfo.window);
    curl_easy_cleanup (appInfo.curl);

    return 0;
}
