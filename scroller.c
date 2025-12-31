/*
 * ============================================================================
 * FLIPPER ZERO - NORTHERN HEMISPHERE STAR MAP SCROLLER
 * ============================================================================
 * 
 * A minimal star map viewer for navigating a polar-projected map of the
 * Northern Hemisphere night sky. The map consists of 50 tiles (5x10 grid)
 * showing stars from magnitude 1-6.
 * 
 * Features:
 * - 640x640px polar projection centered on Polaris
 * - CSV-based tile arrangement and star annotations
 * - Smooth scrolling with arrow keys
 * - 8px cursor circle for star selection
 * - Real-time annotation display for major stars
 * 
 * Author: F Greil
 * License: Open Source
 * ============================================================================
 */

/* ============================================================================
 * SYSTEM INCLUDES
 * ============================================================================ */

#include <furi.h>                               // Core Flipper OS functions
#include <gui/gui.h>                            // GUI rendering and canvas
#include <input/input.h>                        // Input handling (buttons)
#include <storage/storage.h>                    // SD card file access
#include <toolbox/stream/file_stream.h>         // File stream operations
#include <toolbox/stream/buffered_file_stream.h> // Buffered file reading

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ============================================================================ */

// Display dimensions (Flipper Zero screen)
#define SCREEN_WIDTH 128                        // Screen width in pixels
#define SCREEN_HEIGHT 64                        // Screen height in pixels

// Tile dimensions
#define TILE_WIDTH 128                          // Each tile is 128px wide
#define TILE_HEIGHT 64                          // Each tile is 64px tall

// Cursor settings
#define CURSOR_RADIUS 4                         // Cursor circle radius (8px diameter)

// Memory limits
#define MAX_ANNOTATIONS 200                     // Maximum number of star annotations
#define MAX_TILES 25                            // Maximum number of tiles (actually 50, but can expand)
#define MAX_LINE_LENGTH 256                     // Maximum CSV line length
#define MAX_ANNOTATION_LENGTH 64                // Maximum length of star name

// Map dimensions
#define MAP_SIZE 640                            // Star map: 640x640px square
                                                // = 5 columns x 10 rows of 128x64 tiles

/* ============================================================================
 * DATA STRUCTURES
 * ============================================================================ */

/**
 * @brief Represents a single tile in the star map grid
 * 
 * Each tile is a 128x64px image that forms part of the larger 640x640px map.
 * Tiles are arranged in a grid and referenced by row/column position.
 */
typedef struct {
    int row;                                    // Tile row position (0-9)
    int col;                                    // Tile column position (0-4)
    char filename[32];                          // PNG filename (e.g., "tile_0_0.png")
} TileInfo;

/**
 * @brief Represents a star annotation at a specific location
 * 
 * Annotations mark the positions of stars on the map. Each annotation is
 * tied to a specific tile and has x,y coordinates within that tile.
 */
typedef struct {
    char tile_name[32];                         // Which tile this star is on
    int x;                                      // X coordinate within tile (0-127)
    int y;                                      // Y coordinate within tile (0-63)
    char text[MAX_ANNOTATION_LENGTH];           // Star name (e.g., "Polaris (α UMi)")
} Annotation;

/**
 * @brief Main application state
 * 
 * Contains all the data needed for the star map viewer including:
 * - GUI components (viewport, event queue)
 * - Camera position for scrolling
 * - Tile map data
 * - Star annotations
 * - Current selection state
 */
typedef struct {
    // GUI components
    ViewPort* view_port;                        // Flipper viewport for rendering
    FuriMessageQueue* event_queue;              // Queue for input events
    
    // Camera/viewport position (in world coordinates)
    float camera_x;                             // Camera X position (0 to MAP_SIZE-SCREEN_WIDTH)
    float camera_y;                             // Camera Y position (0 to MAP_SIZE-SCREEN_HEIGHT)
    
    // Tile map data
    TileInfo tiles[MAX_TILES];                  // Array of all tiles in the map
    int tile_count;                             // Number of tiles loaded
    int map_width;                              // Map width in tiles (should be 5)
    int map_height;                             // Map height in tiles (should be 10)
    
    // Star annotations
    Annotation annotations[MAX_ANNOTATIONS];    // Array of all star annotations
    int annotation_count;                       // Number of annotations loaded
    
    // Current selection state
    char current_annotation[MAX_ANNOTATION_LENGTH]; // Currently displayed star name
    bool has_annotation;                        // True if cursor is over a star
} ScrollerState;

/* ============================================================================
 * HELPER FUNCTIONS - CSV PARSING
 * ============================================================================ */

/**
 * @brief Parse a CSV line into individual fields
 * 
 * Handles quoted fields and commas within quotes. Splits a single CSV line
 * into separate field strings.
 * 
 * @param line          Input CSV line to parse
 * @param fields        Output array to store parsed fields
 * @param max_fields    Maximum number of fields to parse
 * @return              Number of fields successfully parsed
 */

static int parse_csv_line(const char* line, char fields[][MAX_LINE_LENGTH], int max_fields) {
    int field_count = 0;                        // Number of fields parsed so far
    int pos = 0;                                // Current position in input line
    int field_pos = 0;                          // Current position in current field
    bool in_quotes = false;                     // True when inside quoted string
    
    // Process each character in the line
    while(line[pos] != '\0' && line[pos] != '\n' && line[pos] != '\r' && field_count < max_fields) {
        if(line[pos] == '"') {
            // Toggle quote state - allows commas inside quotes
            in_quotes = !in_quotes;
            pos++;
        } else if(line[pos] == ',' && !in_quotes) {
            // Comma outside quotes = field separator
            fields[field_count][field_pos] = '\0'; // Null-terminate current field
            field_count++;
            field_pos = 0;
            pos++;
        } else {
            // Regular character - add to current field
            fields[field_count][field_pos++] = line[pos++];
            
            // Safety check: prevent buffer overflow
            if(field_pos >= MAX_LINE_LENGTH - 1) {
                fields[field_count][field_pos] = '\0';
                field_count++;
                field_pos = 0;
                break;
            }
        }
    }
    
    // Handle last field (if any data remains)
    if(field_pos > 0 || (pos > 0 && line[pos-1] == ',')) {
        fields[field_count][field_pos] = '\0';
        field_count++;
    }
    
    return field_count;
}

/* ============================================================================
 * HELPER FUNCTIONS - FILE LOADING
 * ============================================================================ */

/**
 * @brief Load tile map configuration from CSV file
 * 
 * Reads assets/tiles.csv to determine the arrangement of tiles in the star map.
 * Each line specifies a tile's row, column, and image filename.
 * 
 * Expected CSV format:
 *   row,col,filename
 *   0,0,tile_0_0.png
 *   0,1,tile_0_1.png
 *   ...
 * 
 * @param state     Application state to populate with tile data
 * @param storage   Flipper storage API handle
 * @return          true if successful, false on error
 */

static bool load_tile_map(ScrollerState* state, Storage* storage) {
    // Open buffered file stream for efficient reading
    Stream* stream = buffered_file_stream_alloc(storage);
    bool success = false;
    
    // Try to open tiles.csv from SD card
    if(!buffered_file_stream_open(stream, APP_DATA_PATH("scroller") "/assets/tiles.csv", FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E("Scroller", "Failed to open assets/tiles.csv");
        buffered_file_stream_close(stream);
        stream_free(stream);
        return false;
    }
    
    char line[MAX_LINE_LENGTH];
    char fields[10][MAX_LINE_LENGTH];
    state->tile_count = 0;
    state->map_width = 0;
    state->map_height = 0;
    
    // Skip header line (row,col,filename)
    stream_read_line(stream, line, MAX_LINE_LENGTH);
    
    // Read each tile definition
    while(stream_read_line(stream, line, MAX_LINE_LENGTH) && state->tile_count < MAX_TILES) {
        int field_count = parse_csv_line(line, fields, 10);
        
        // Need at least 3 fields: row, col, filename
        if(field_count >= 3) {
            TileInfo* tile = &state->tiles[state->tile_count];
            
            // Parse tile properties
            tile->row = atoi(fields[0]);        // Convert row string to integer
            tile->col = atoi(fields[1]);        // Convert column string to integer
            strncpy(tile->filename, fields[2], sizeof(tile->filename) - 1);
            tile->filename[sizeof(tile->filename) - 1] = '\0'; // Ensure null termination
            
            // Update map dimensions based on tile positions
            if(tile->col + 1 > state->map_width) state->map_width = tile->col + 1;
            if(tile->row + 1 > state->map_height) state->map_height = tile->row + 1;
            
            state->tile_count++;
        }
    }
    
    success = state->tile_count > 0;
    buffered_file_stream_close(stream);
    stream_free(stream);
    
    // Log success message with tile count and map dimensions
    FURI_LOG_I("Scroller", "Loaded %d tiles, map: %dx%d", state->tile_count, state->map_width, state->map_height);
    return success;
}

/**
 * @brief Load star annotations from CSV file
 * 
 * Reads assets/annotations.csv to get positions and names of stars.
 * Each line specifies which tile a star is on, its coordinates within
 * that tile, and the star's name.
 * 
 * Expected CSV format:
 *   tile_name,x,y,annotation
 *   tile_4_2.png,64,32,Polaris (α UMi)
 *   tile_5_1.png,68,42,Capella (α Aur)
 *   ...
 * 
 * @param state     Application state to populate with annotation data
 * @param storage   Flipper storage API handle
 * @return          true if successful, false on error
 */

static bool load_annotations(ScrollerState* state, Storage* storage) {
    // Open buffered file stream for efficient reading
    Stream* stream = buffered_file_stream_alloc(storage);
    bool success = false;
    
    // Try to open annotations.csv from SD card
    if(!buffered_file_stream_open(stream, APP_DATA_PATH("scroller") "/assets/annotations.csv", FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E("Scroller", "Failed to open assets/annotations.csv");
        buffered_file_stream_close(stream);
        stream_free(stream);
        return false;
    }
    
    char line[MAX_LINE_LENGTH];
    char fields[10][MAX_LINE_LENGTH];
    state->annotation_count = 0;
    
    // Skip header line (tile_name,x,y,annotation)
    stream_read_line(stream, line, MAX_LINE_LENGTH);
    
    // Read each annotation (star position and name)
    while(stream_read_line(stream, line, MAX_LINE_LENGTH) && state->annotation_count < MAX_ANNOTATIONS) {
        int field_count = parse_csv_line(line, fields, 10);
        
        // Need at least 4 fields: tile_name, x, y, annotation
        if(field_count >= 4) {
            Annotation* ann = &state->annotations[state->annotation_count];
            
            // Parse annotation properties
            strncpy(ann->tile_name, fields[0], sizeof(ann->tile_name) - 1);
            ann->tile_name[sizeof(ann->tile_name) - 1] = '\0'; // Ensure null termination
            ann->x = atoi(fields[1]);           // X coordinate within tile
            ann->y = atoi(fields[2]);           // Y coordinate within tile
            strncpy(ann->text, fields[3], sizeof(ann->text) - 1);
            ann->text[sizeof(ann->text) - 1] = '\0'; // Ensure null termination
            
            state->annotation_count++;
        }
    }
    
    success = true;
    buffered_file_stream_close(stream);
    stream_free(stream);
    
    // Log success message with annotation count
    FURI_LOG_I("Scroller", "Loaded %d annotations", state->annotation_count);
    return success;
}

/* ============================================================================
 * HELPER FUNCTIONS - ANNOTATION DETECTION
 * ============================================================================ */

/**
 * @brief Check if the cursor is currently over any star annotation
 * 
 * This function performs collision detection between the cursor circle
 * (always at screen center) and star annotation points. When the cursor
 * overlaps a star's position, that star's name is displayed.
 * 
 * Process:
 * 1. Calculate cursor position in world coordinates
 * 2. Determine which tile the cursor is on
 * 3. Calculate cursor position within that tile
 * 4. Check all annotations for that tile
 * 5. Use circular collision detection (distance <= radius)
 * 
 * @param state     Application state containing camera position and annotations
 */

static void check_annotations(ScrollerState* state) {
    // Reset annotation state
    state->has_annotation = false;
    state->current_annotation[0] = '\0';
    
    // Calculate cursor position in world coordinates
    // Cursor is always at center of screen, so we add half screen dimensions to camera position
    int cursor_world_x = (int)(state->camera_x + SCREEN_WIDTH / 2);
    int cursor_world_y = (int)(state->camera_y + SCREEN_HEIGHT / 2);
    
    // Determine which tile the cursor is on
    int cursor_tile_col = cursor_world_x / TILE_WIDTH;  // Integer division gives tile column
    int cursor_tile_row = cursor_world_y / TILE_HEIGHT; // Integer division gives tile row
    
    // Find the tile at cursor position
    TileInfo* current_tile = NULL;
    for(int i = 0; i < state->tile_count; i++) {
        if(state->tiles[i].row == cursor_tile_row && state->tiles[i].col == cursor_tile_col) {
            current_tile = &state->tiles[i];
            break;
        }
    }
    
    // If no tile at cursor position, no annotation possible
    if(!current_tile) return;
    
    // Calculate cursor position within the current tile
    // Modulo operation gives remainder = position within tile
    int tile_local_x = cursor_world_x % TILE_WIDTH;
    int tile_local_y = cursor_world_y % TILE_HEIGHT;
    
    // Check all annotations for the current tile
    for(int i = 0; i < state->annotation_count; i++) {
        Annotation* ann = &state->annotations[i];
        
        // Only check annotations that belong to the current tile
        if(strcmp(ann->tile_name, current_tile->filename) == 0) {
            // Calculate distance between cursor and annotation point
            int dx = tile_local_x - ann->x;
            int dy = tile_local_y - ann->y;
            int dist_sq = dx * dx + dy * dy;   // Distance squared (avoid sqrt for efficiency)
            
            // Check if cursor circle overlaps annotation point
            // Using squared distance to avoid expensive square root calculation
            if(dist_sq <= CURSOR_RADIUS * CURSOR_RADIUS) {
                // Found an annotation within cursor radius!
                state->has_annotation = true;
                strncpy(state->current_annotation, ann->text, sizeof(state->current_annotation) - 1);
                state->current_annotation[sizeof(state->current_annotation) - 1] = '\0';
                break; // Only show first annotation found
            }
        }
    }
}

/* ============================================================================
 * GUI CALLBACKS
 * ============================================================================ */

/**
 * @brief Canvas draw callback - renders the star map and UI
 * 
 * Called by the Flipper OS whenever the screen needs to be redrawn.
 * Renders visible tiles, cursor, and any active annotation.
 * 
 * Drawing steps:
 * 1. Clear canvas
 * 2. Calculate which tiles are visible
 * 3. Draw each visible tile (currently just borders for debugging)
 * 4. Draw cursor circle at screen center
 * 5. Draw annotation text if cursor is over a star
 * 
 * @param canvas    Flipper canvas API for drawing
 * @param ctx       Application state (ScrollerState*)
 */

static void scroller_draw_callback(Canvas* canvas, void* ctx) {
    ScrollerState* state = (ScrollerState*)ctx;
    
    // Clear the entire canvas to black
    canvas_clear(canvas);
    
    // Calculate which tiles are currently visible on screen
    // We need to draw tiles that overlap with the camera viewport
    int start_tile_col = (int)(state->camera_x / TILE_WIDTH);
    int start_tile_row = (int)(state->camera_y / TILE_HEIGHT);
    int end_tile_col = (int)((state->camera_x + SCREEN_WIDTH) / TILE_WIDTH) + 1;
    int end_tile_row = (int)((state->camera_y + SCREEN_HEIGHT) / TILE_HEIGHT) + 1;
    
    // Draw all visible tiles
    canvas_set_color(canvas, ColorBlack);
    for(int i = 0; i < state->tile_count; i++) {
        TileInfo* tile = &state->tiles[i];
        
        // Check if this tile is within the visible range
        if(tile->col >= start_tile_col && tile->col <= end_tile_col &&
           tile->row >= start_tile_row && tile->row <= end_tile_row) {
            
            // Convert tile position from world coordinates to screen coordinates
            int screen_x = (int)(tile->col * TILE_WIDTH - state->camera_x);
            int screen_y = (int)(tile->row * TILE_HEIGHT - state->camera_y);
            
            // Draw tile border (debug visualization)
            // In production, this would load and draw the actual PNG tile image
            canvas_draw_frame(canvas, screen_x, screen_y, TILE_WIDTH, TILE_HEIGHT);
            
            // Draw "STARS" text as placeholder for actual star field image
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, screen_x + 2, screen_y + 8, "STARS");
        }
    }
    
    // Draw cursor circle at center of screen
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_circle(canvas, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, CURSOR_RADIUS);
    
    // If cursor is over a star annotation, draw the star name
    if(state->has_annotation) {
        canvas_set_font(canvas, FontSecondary);
        canvas_set_color(canvas, ColorBlack);
        
        // Draw background box for better text readability
        int text_width = canvas_string_width(canvas, state->current_annotation);
        canvas_draw_box(canvas, 0, 0, text_width + 4, 10);
        
        // Draw star name in white on black background
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str(canvas, 2, 8, state->current_annotation);
        
        // Draw "OK" indicator at bottom-right to show annotation is active
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, SCREEN_WIDTH - 18, SCREEN_HEIGHT - 2, "OK");
    }
}

/**
 * @brief Input event callback - receives button presses
 * 
 * Called by the Flipper OS when a button is pressed. Puts the event
 * into the message queue for processing in the main loop.
 * 
 * @param input_event   Button press/release event
 * @param ctx           Event queue (FuriMessageQueue*)
 */

static void scroller_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    // Forward input event to message queue for processing in main loop
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

/* ============================================================================
 * MAIN APPLICATION ENTRY POINT
 * ============================================================================ */

/**
 * @brief Main application entry point
 * 
 * This is the entry point defined in application.fam. The Flipper OS calls
 * this function when the app is launched from the menu.
 * 
 * Main tasks:
 * 1. Initialize application state
 * 2. Load CSV data files (tiles and annotations)
 * 3. Set up GUI (viewport, callbacks)
 * 4. Run main event loop (handle button presses, update display)
 * 5. Clean up and exit
 * 
 * @param p     Unused parameter (required by Flipper API)
 * @return      Exit code (0 = success)
 */

int32_t scroller_main(void* p) {
    UNUSED(p);
    
    /* ------------------------------------------------------------------------
     * INITIALIZATION
     * ------------------------------------------------------------------------ */
    
    // Allocate and initialize application state
    ScrollerState* state = malloc(sizeof(ScrollerState));
    memset(state, 0, sizeof(ScrollerState));
    
    // Initialize camera to center of map
    // This starts the view centered on Polaris (North Star)
    state->camera_x = (MAP_SIZE - SCREEN_WIDTH) / 2.0f;   // Center horizontally
    state->camera_y = (MAP_SIZE - SCREEN_HEIGHT) / 2.0f;  // Center vertically
    
    // Open storage API for reading CSV files from SD card
    Storage* storage = furi_record_open(RECORD_STORAGE);
    
    // Load tile map configuration (which tiles exist and where)
    if(!load_tile_map(state, storage)) {
        FURI_LOG_E("Scroller", "Failed to load tile map");
        // Continue anyway - app will still run with no tiles
    }
    
    // Load star annotations (positions and names)
    if(!load_annotations(state, storage)) {
        FURI_LOG_E("Scroller", "Failed to load annotations");
        // Continue anyway - app will still run with no annotations
    }
    
    // Close storage - we're done reading files
    furi_record_close(RECORD_STORAGE);
    
    /* ------------------------------------------------------------------------
     * GUI SETUP
     * ------------------------------------------------------------------------ */
    
    // Create message queue for input events (button presses)
    state->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    
    // Create viewport for rendering
    state->view_port = view_port_alloc();
    
    // Register draw callback - called when screen needs redrawing
    view_port_draw_callback_set(state->view_port, scroller_draw_callback, state);
    
    // Register input callback - called when buttons are pressed
    view_port_input_callback_set(state->view_port, scroller_input_callback, state->event_queue);
    
    // Register viewport with GUI system in fullscreen mode
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, state->view_port, GuiLayerFullscreen);
    
    /* ------------------------------------------------------------------------
     * MAIN EVENT LOOP
     * ------------------------------------------------------------------------ */
    
    InputEvent event;
    bool running = true;
    const float move_speed = 4.0f;              // Camera movement speed in pixels
    
    // Check for annotations at initial position
    check_annotations(state);
    view_port_update(state->view_port);         // Request initial draw
    
    // Main loop - processes input events until user exits
    while(running) {
        // Wait up to 100ms for an input event
        if(furi_message_queue_get(state->event_queue, &event, 100) == FuriStatusOk) {
            // Only process press and repeat events (ignore release)
            if(event.type == InputTypePress || event.type == InputTypeRepeat) {
                switch(event.key) {
                    case InputKeyUp:
                        // Move camera up (decrease Y)
                        state->camera_y -= move_speed;
                        if(state->camera_y < 0) state->camera_y = 0;
                        break;
                        
                    case InputKeyDown:
                        // Move camera down (increase Y)
                        state->camera_y += move_speed;
                        if(state->camera_y > MAP_SIZE - SCREEN_HEIGHT) {
                            state->camera_y = MAP_SIZE - SCREEN_HEIGHT;
                        }
                        break;
                        
                    case InputKeyLeft:
                        // Move camera left (decrease X)
                        state->camera_x -= move_speed;
                        if(state->camera_x < 0) state->camera_x = 0;
                        break;
                        
                    case InputKeyRight:
                        // Move camera right (increase X)
                        state->camera_x += move_speed;
                        if(state->camera_x > MAP_SIZE - SCREEN_WIDTH) {
                            state->camera_x = MAP_SIZE - SCREEN_WIDTH;
                        }
                        break;
                        
                    case InputKeyBack:
                        // Back button - exit application
                        running = false;
                        break;
                        
                    case InputKeyOk:
                        // OK button - could show more info about selected star
                        if(state->has_annotation) {
                            FURI_LOG_I("Scroller", "Selected: %s", state->current_annotation);
                        }
                        break;
                        
                    default:
                        break;
                }
                
                // After camera movement, check for new annotations at cursor position
                check_annotations(state);
                
                // Request screen redraw with new camera position
                view_port_update(state->view_port);
            }
        }
    }
    
    /* ------------------------------------------------------------------------
     * CLEANUP
     * ------------------------------------------------------------------------ */
    
    // Remove viewport from GUI
    gui_remove_view_port(gui, state->view_port);
    furi_record_close(RECORD_GUI);
    
    // Free viewport
    view_port_free(state->view_port);
    
    // Free message queue
    furi_message_queue_free(state->event_queue);
    
    // Free application state
    free(state);
    
    return 0;
}
