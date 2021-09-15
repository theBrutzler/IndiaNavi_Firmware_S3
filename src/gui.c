/*
 * GUI for the IndiaNavi Applcation
 *
 * Handles GuiTask and DisplayManager
 *
 *  Created on: Jan 6, 2021
 *      Author: bastian
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <hw/regulator_gpio.h>
#include "gui.h"
#include "tasks.h"
#include "pins.h"

#include "time.h"
#include <sys/time.h>

#include <driver/gpio.h>

#include "icons_32/icons_32.h"

const uint16_t margin_top = 5;
const uint16_t margin_bottom = 5;
const uint16_t margin_vertical = 10;
const uint16_t margin_left = 5;
const uint16_t margin_right = 5;
const uint16_t margin_horizontal = 10;

static const char *TAG = "GUI";

static display_t *eink;

extern void vTaskGetRunTimeStats(char *pcWriteBuffer);

render_t *render_pipeline[RL_MAX]; // maximum number of rendered items
render_t *render_last[RL_MAX]; // pointer to end of render pipeline
static uint8_t render_needed = 0;

/**
 * Add render function to pipeline
 *
 * @return render slot
 */
render_t *add_to_render_pipeline(error_code_t (*render)(display_t *dsp, void *component), 
	void *comp, 
	enum RenderLayer layer)
{
	// increase render slot before adding this one. Slot 0 is overflow!
	render_t *rd = RTOS_Malloc(sizeof(render_t));
	if (!rd)
	{
		ESP_LOGE(TAG, "render pipeline full!");
		return 0;
	}
	rd->render = render;
	rd->comp = comp;
	
	if(render_pipeline[layer] == NULL){
		render_pipeline[layer] = rd;
	} else {
		render_last[layer]->next = rd;
	}
	render_last[layer] = rd;

	return rd;
}

void free_render_pipeline(enum RenderLayer layer){
	render_t *r = render_pipeline[layer];
	while(r){
		render_t *rn;
		rn = r;
		r = r->next;
		free(rn);
	}
}

static label_t *create_icon_with_text(display_t *dsp, uint8_t *icon_data,
									  uint16_t left, uint16_t top, char *text, font_t *font)
{

	image_t *img = image_create(icon_data, left, top, ICON_SIZE,
								ICON_SIZE);

	label_t *il = label_create(text, font,
							   img->box.left + img->box.width + margin_left, top, 0, 0);
	il->child = img;
	label_shrink_to_text(il);
	il->alignVertical = MIDDLE;
	add_to_render_pipeline(label_render, il, RL_GUI_ELEMENTS);
	// render image after Label is rendered
	add_to_render_pipeline(image_render, img, RL_GUI_ELEMENTS);
	return il;
}

/**
 * Callbacks from renderer for clock label
 */
error_code_t updateTimeText(display_t *dsp, void *comp)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	struct tm *timeinfo = localtime(&tv.tv_sec);
	xSemaphoreTake(print_semaphore, portMAX_DELAY);
	sprintf(clock_label->text, "%02d:%02d", timeinfo->tm_hour,
			timeinfo->tm_min);
	xSemaphoreGive(print_semaphore);
	return PM_OK;
}

static void create_top_bar(display_t *dsp)
{
	label_t *sb = label_create("", &f8x8, 0, 0, (dsp->size.width - 1),
							   ICON_SIZE + margin_vertical);

	sb->borderColor = BLACK;
	sb->borderWidth = 1;
	sb->borderLines = ALL_SOLID;
	sb->alignHorizontal = CENTER;
	sb->alignVertical = MIDDLE;
	sb->backgroundColor = WHITE;
	add_to_render_pipeline(label_render, sb, RL_GUI_BACKGROUND);

	/* TODO: export battery component */
	
	battery_label = create_icon_with_text(dsp, bat_100,
										  sb->box.left + margin_left, margin_top, RTOS_Malloc(4), &f8x8);
	save_sprintf(battery_label->text, "...%%");
	label_shrink_to_text(battery_label);

	north_indicator_label = create_icon_with_text(dsp, norden,
												  battery_label->box.left + battery_label->box.width + margin_horizontal,
												  margin_top, "", &f8x8);

	wifi_indicator_label = create_icon_with_text(dsp, WIFI_0,
												 north_indicator_label->box.left + north_indicator_label->box.width + margin_horizontal,
												 margin_top, "", &f8x8);

	char *GPSView = RTOS_Malloc(5);
	gps_indicator_label = create_icon_with_text(dsp, noGPS,
												dsp->size.width - ICON_SIZE - (2 * margin_right) - 8, margin_top, GPSView, &f8x8);
	sd_indicator_label = create_icon_with_text(dsp, noSD,
											   gps_indicator_label->box.left - 2 * ICON_SIZE - margin_right, margin_top, "",
											   &f8x8);

	/* global clock label. */
	char *time = RTOS_Malloc(6);
	clock_label = label_create(time, &f8x8, sb->box.left, sb->box.top,
							   sb->box.width, sb->box.height);
	clock_label->alignVertical = MIDDLE;
	clock_label->alignHorizontal = CENTER;
	clock_label->onBeforeRender = updateTimeText;
	add_to_render_pipeline(label_render, clock_label, RL_GUI_ELEMENTS);
}

/**
 * Render all App components.
 *
 */
static void app_render()
{
	render_t *rd;
	uint64_t start = esp_timer_get_time();
	display_fill(eink, WHITE);
	for (uint8_t layer = 0; layer < RL_MAX; layer++)
	{
		rd = render_pipeline[layer];
		while(rd){
			if (rd->render)
				rd->render(eink, rd->comp);
			rd = rd->next;
		}
		vTaskDelay(0);
	}

	uint64_t end = esp_timer_get_time();

	ESP_LOGI(TAG, "render time %i ms", (uint32_t)(end - start) / 1000);
}

void wait_until_gui_ready()
{
	/* map tiles */
	for (uint8_t i = 0; i < 6; i++)
	{
		while (!map_tiles[i].image)
			vTaskDelay(0);
	}
	while (!positon_marker)
	{
		vTaskDelay(0);
	}
}

/*
 * Load tile data from SD Card on render command
 *
 * We share a global memory buffer since it is not enough memory available
 * to hold all 6 tiles in memory at once.
 * 
 * returns PK_OK if loaded, UNAVAILABLE if buffer of sd semaphore is not available and
 * TIMEOUT if loading timed out
 */
error_code_t load_map_tile_on_demand(display_t *dsp, void *image)
{
	uint8_t timeout = 0;
	uint8_t *imageBuf = RTOS_Malloc(256 * 256 / 2);
	ESP_LOGI(TAG, "Image at: 0x%X", (uint)imageBuf);
	if (!imageBuf)
		return UNAVAILABLE;

	image_t *img = (image_t *)image;
	if (!uxSemaphoreGetCount(sd_semaphore))
	{ // binary semaphore returns 1 on not taken
		RTOS_Free(imageBuf);
		return UNAVAILABLE;
	}

	img->data = imageBuf;
	loadTile(img->parent); // queue loading

	while (img->loaded != LOADED)
	{
		label_t *l = (label_t*)img->child;
		if (img->loaded == ERROR){
			l->text = "Error";
			goto freeImageMemory;
		}
		if (img->loaded == NOT_FOUND){
			//l->text = "Not Found";
			goto freeImageMemory;
		}
		vTaskDelay(10);
		timeout++;
		if (timeout == 100)
		{
			ESP_LOGI(TAG, "load timeout!");
			goto freeImageMemory;
		}
	}

	return PM_OK;

freeImageMemory:
	RTOS_Free(imageBuf);
	return TIMEOUT;
}

error_code_t check_if_map_tile_is_loaded(display_t *dsp, void *image)
{
	image_t *img = (image_t *)image;
	label_t *lbl = img->child;
	if (img->loaded != LOADED) {
		label_render(dsp, lbl);
	} else {
		RTOS_Free(img->data);
	}
	return PM_OK;
}

/**
 * Display App
 */
void app_screen(display_t *dsp)
{
	//imageBuf = RTOS_Malloc(256 * 256 / 2);
	// we first create the map_tiles to render them on the lowest level
	for (uint8_t i = 0; i < 2; i++)
		for (uint8_t j = 0; j < 3; j++)
		{
			uint8_t idx = i * 3 + j;
			map_tiles[idx].image = image_create(NULL, i * 255, j * 255, 256,
												256);
			map_tiles[idx].label = label_create(RTOS_Malloc(21), &f8x8, i * 255,
												j * 255, 256, 256);
			save_sprintf(map_tiles[idx].label->text, "loading...");

			map_tiles[idx].label->alignHorizontal = CENTER;
			map_tiles[idx].label->alignVertical = MIDDLE;
			map_tiles[idx].label->backgroundColor = WHITE;
			map_tiles[idx].image->onBeforeRender = load_map_tile_on_demand;
			map_tiles[idx].image->onAfterRender = check_if_map_tile_is_loaded;
			map_tiles[idx].image->child = map_tiles[idx].label;
			map_tiles[idx].image->loaded = 0;
			map_tiles[idx].image->parent = &map_tiles[idx];
			map_tiles[idx].x = i;
			map_tiles[idx].y = j;
			add_to_render_pipeline(image_render, map_tiles[idx].image, RL_MAP);
		}

	/* position marker */
	positon_marker = label_create("", &f8x16, 0, 0, 24, 24);
	positon_marker->textColor = BLUE;
	positon_marker->alignHorizontal = CENTER;
	positon_marker->alignVertical = MIDDLE;
	add_to_render_pipeline(label_render, positon_marker, RL_TOP);

	/* scale 63px for 100m | 96px for 500ft @ zoom 16*/
	/* scale 77px for 500m | 94px for 2000ft @zoom 14 */
	scaleBox = label_create("100m", &f8x8, 10, dsp->size.height - 34, 63, 13);
	scaleBox->borderWidth = 1;
	scaleBox->borderLines = LEFT_SOLID | RIGHT_SOLID | BOTTOM_SOLID;
	//scaleBox->backgroundColor = WHITE;
	scaleBox->borderColor = BLACK;
	scaleBox->textColor = BLACK;
	scaleBox->alignVertical = BOTTOM;
	scaleBox->alignHorizontal = CENTER;
	add_to_render_pipeline(label_render, scaleBox, RL_TOP);


	create_top_bar(dsp);

	char *infoText = RTOS_Malloc(dsp->size.width / f8x8.width);
	xSemaphoreTake(print_semaphore, portMAX_DELAY);

	sprintf(infoText, GIT_HASH);
	xSemaphoreGive(print_semaphore);
	infoBox = label_create(infoText, &f8x8, 0, dsp->size.height - 14,
						   dsp->size.width - 1, 13);
	infoBox->borderWidth = 1;
	infoBox->borderLines = ALL_SOLID;
	infoBox->alignVertical = MIDDLE;
	infoBox->backgroundColor = WHITE;

	add_to_render_pipeline(label_render, infoBox, RL_GUI_ELEMENTS);

	label_t *map_copyright = label_create("(c) OpenStreetMap contributors", &f8x8, 0,infoBox->box.top - 8, 0,0);
	label_shrink_to_text(map_copyright);
	map_copyright->box.left = dsp->size.width - map_copyright->box.width;
	add_to_render_pipeline(label_render, map_copyright, RL_GUI_ELEMENTS);

}

void trigger_rendering()
{
	render_needed = 1;
}

void render_cmd_cb(command_t *cmd)
{
	ESP_LOGI(TAG, "manual redraw");
	trigger_rendering();
}

void StartGuiTask(void const *argument)
{
	ESP_LOGI(TAG, "init");
	ESP_LOGI(TAG, "fonts loading");
	font_load_from_array(&f8x8, font8x8, font8x8_name);
	font_load_from_array(&f8x16, font8x16, font8x16_name);

	//gpio_t *eeprom = gpio_create(OUTPUT, 0, EINK_EEPROM_nEN);
	//eeprom->onValue = GPIO_RESET;
	//gpio_write(eeprom, GPIO_SET);

	ESP_LOGI(TAG, "init Display regualtor");
	gpio_t *reg_gpio = gpio_create(OUTPUT, 0, EINK_VCC_nEN);
	reg_gpio->onValue = GPIO_RESET;
	regulator_t *reg = regulator_gpio_create(reg_gpio);

	ESP_LOGI(TAG, "init E-Ink Display");
	do {


		reg->disable(reg);
		vTaskDelay(500);
		reg->enable(reg);
		vTaskDelay(10);
		eink = ACEP_5IN65_Init(DISPLAY_ROTATE_90);
		if (!eink){
			ESP_LOGE(TAG, "E-Ink Display not initialized! retry...");
		}
	} while (!eink);

	ESP_LOGI(TAG, "App screen init");

	//ESP_LOGI(TAG, "Screen clear");
	//display_fill(eink, TRANSPARENT);
	//display_commit_fb(eink);
	//ESP_LOGI(TAG, "Screen clear done");

	app_screen(eink);
	ESP_LOGI(TAG, "App screen init done");

	/*	command_t cmd;
	cmd.command = "redraw";
	cmd.function_cb = render_cmd_cb;
	command_register(&cmd);
	ESP_LOGI(TAG, "command %s registered", cmd.command);
*/
	vTaskDelay(100 / portTICK_PERIOD_MS);
	ESP_LOGI(TAG, "Loop ready.");

	for (;;)
	{
		/* render trigger TODO: have a way to not do it if other tasks want us to wait*/
		if (render_needed)
		{
			if (xSemaphoreTake(gui_semaphore, 0) == pdTRUE)
			{
				xSemaphoreGive(gui_semaphore);
				pre_render_cb();
				app_render();
				render_needed = 0;
				ESP_LOGI(TAG, "Refresh.");
				//vTaskPrioritySet(NULL, 1);
				display_commit_fb(eink);
				//vTaskPrioritySet(NULL, 5);
				ESP_LOGI(TAG, "Refresh finished.");
			}
			else
			{
				ESP_LOGI(TAG, "Render Mutex locked.");
			}
		}
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}
