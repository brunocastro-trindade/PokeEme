#ifdef PLATFORM_SDL2
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#ifdef __SWITCH__
#include <sys/stat.h>
#include <switch.h>
#endif

#include <SDL2/SDL.h>

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "platform/dma.h"
#include "platform/field3d.h"
#include "platform/follower_gfx.h"
#include "platform/framedraw.h"
#include "platform/gl_field3d.h"
#include "platform/gl_renderer.h"
#include "platform/harness.h"
#include "platform/hd_menu.h"
#include "platform/hd_settings.h"
#include "platform/hd_sprites.h"
#include "platform/ppu_layers.h"
#include "platform/system.h"
#include "platform/test_hooks.h"

extern void (*const gIntrTable[])(void);

SDL_Thread *mainLoopThread;
SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_sem *vBlankSemaphore;
SDL_atomic_t isFrameAvailable;
bool speedUp = false;
unsigned int videoScale = 1;
bool videoScaleChanged = false;
bool isRunning = true;
bool paused = false;
double simTime = 0;
double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
double timeScale = 1.0;
struct SiiRtcInfo internalClock;

static FILE *sSaveFile = NULL;

extern void AgbMain(void);
extern void MainLoop(void);
extern void DoSoftReset(void);

int DoMain(void *param);
void ProcessEvents(void);
static const uint16_t *RenderGameFrame(void);
static bool CreateGlOutput(int width, int height, Uint32 windowFlags);
static bool CreateSdlRendererOutput(int width, int height, Uint32 windowFlags);
static void PresentOutput(void);
static void RunHeadless(void);

static uint16_t sFrameImage[DISPLAY_WIDTH * DISPLAY_HEIGHT]; // software-composed frame
static uint16_t sGpuImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];   // GPU-composed frame read back
static struct PpuFrameLayers sLayers;
static SDL_GLContext sGlContext;
static bool sGlActive; // video goes through the OpenGL compositor
static uint32_t sDrawnFrames; // clock for the HD sprite animations
static void InitHdSprites(void);

#ifdef __SWITCH__
#define SWITCH_BUILD true
#else
#define SWITCH_BUILD false
#endif

static void ReadSaveFile(char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);

static void UpdateInternalClock(void);

#define MAX_CONTROLLERS 4
static SDL_GameController *sControllers[MAX_CONTROLLERS];
static bool sControllerSpeedUp;
static void OpenController(int deviceIndex);
static void CloseController(SDL_JoystickID instanceId);
static u16 ReadControllers(void);
static uint32_t ReadMenuButtons(void);
static void UpdateMenu(void);
static void InitSettings(void);
static void DataPath(char *out, size_t size, const char *sub);
static uint32_t sRenderedFrames; // for the frame counter

int main(int argc, char **argv)
{
    int windowWidth, windowHeight;
    Uint32 windowFlags;

    // Open an output console on Windows
#ifdef _WIN32
    AllocConsole() ;
    AttachConsole( GetCurrentProcessId() ) ;
    freopen( "CON", "w", stdout ) ;
#endif

    Harness_Init();
    TestHooks_Init();
#ifdef __SWITCH__
    mkdir("sdmc:/switch/pokeemerald", 0777);
    // what the game prints goes to a file on the SD card (for bug reports)
    {
        static const char sLogPath[] = "sdmc:/switch/pokeemerald/log.txt";
        FILE *log = fopen(sLogPath, "w"); // start it empty
        if (log != NULL)
            fclose(log);
        if (freopen(sLogPath, "a", stdout) != NULL)
            setvbuf(stdout, NULL, _IOLBF, 0);
        if (freopen(sLogPath, "a", stderr) != NULL)
            setvbuf(stderr, NULL, _IONBF, 0);
    }
    printf("pokeemerald HD-2D (%s %s)\n", __DATE__, __TIME__);
    if (R_FAILED(romfsInit()))
        printf("romfs: none in the .nro, data comes from the SD card\n");
#endif
    ReadSaveFile((char *)Harness_SavePath());
    InitSettings();
    {
        char path[1024];
        DataPath(path, sizeof(path), "followers.bin");
        if (!FollowerGfx_Init(path))
            HdSettings_SetProblem(HD_OPT_FOLLOWER, "Sem dados");
    }

    if (Harness_IsHeadless())
    {
        RunHeadless();
        return Harness_Finish() ? 0 : 2;
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#ifdef __SWITCH__
    // The Switch always gets a full-screen 1280x720 window
    windowWidth = 1280;
    windowHeight = 720;
    windowFlags = 0;
#else
    videoScale = 4;
    windowWidth = DISPLAY_WIDTH * videoScale;
    windowHeight = DISPLAY_HEIGHT * videoScale;
    windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#endif

    if (!(Harness_UseGl() && CreateGlOutput(windowWidth, windowHeight, windowFlags))
     && !CreateSdlRendererOutput(windowWidth, windowHeight, windowFlags))
        return 1;

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    isFrameAvailable.value = 0;
    vBlankSemaphore = SDL_CreateSemaphore(0);

    SDL_AudioSpec want;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = 42048;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
    cgb_audio_init(want.freq);


    if (SDL_OpenAudio(&want, 0) < 0)
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    else
    {
        if (want.format != AUDIO_F32) /* we let this one thing change. */
            SDL_Log("We didn't get Float32 audio format.");
        SDL_PauseAudio(0);
    }

    AgbMain();

    double accumulator = 0.0;

    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    bool isGameStepDrawn = false;
    while (isRunning)
    {
        double deltaTime;

        ProcessEvents();
        UpdateMenu();

        curGameTime = SDL_GetPerformanceCounter();
        deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
        deltaTime *= timeScale; //apply speedup

        if (!paused)
        {
            accumulator += deltaTime;

            isGameStepDrawn = false;

            while (accumulator >= fixedTimestep)
            {
                //run game logic, draw frame and process DMAs and vblank
                const uint16_t *image = NULL;

                ENTER_VBLANK(); //you must be in VBlank before running a game tick
                MainLoop();
                TestHooks_OnFrame(Harness_FrameCount());
                if (!isGameStepDrawn)
                {
                    image = RenderGameFrame();
                    isGameStepDrawn = true;
                }
                RunDMAsAndVBlank();
                if (!Harness_EndFrame(image))
                    isRunning = false;

                accumulator -= fixedTimestep;
            }

            //samples per frame is 701, that gets multipled by two when being queued and then multipled by four because samples are float32 which are 4 bytes long hence the divide by 8
            //this number is then checked against samples per frame multipled by three rounded down to 2000 to give it enough margin of error while not desyncing
            //this is all done to sync audio to gameplay
            if (SDL_GetQueuedAudioSize(1)/8 < 2000)
            {
                AudioUpdate();
            }

            // The SDL_Renderer output snaps the window to integer scales;
            // the GL compositor scales smoothly to any size.
            if (videoScaleChanged && !sGlActive && !SWITCH_BUILD)
            {
                SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
                videoScaleChanged = false;
            }
        }

        lastGameTime = curGameTime;

        PresentOutput();
    }

    Harness_Finish();
    CloseSaveFile();

    if (sGlActive)
    {
        GlRenderer_Shutdown();
        SDL_GL_DeleteContext(sGlContext);
    }
    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}

static bool CreateGlOutput(int width, int height, Uint32 windowFlags)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 width, height, windowFlags | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (sdlWindow == NULL)
    {
        fprintf(stderr, "gl: window creation failed: %s\n", SDL_GetError());
        return false;
    }
    sGlContext = SDL_GL_CreateContext(sdlWindow);
    if (sGlContext == NULL)
    {
        fprintf(stderr, "gl: context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(sdlWindow);
        sdlWindow = NULL;
        return false;
    }
    if (!GlRenderer_Init(SDL_GL_GetProcAddress))
    {
        fprintf(stderr, "gl: renderer setup failed, falling back to SDL_Renderer\n");
        SDL_GL_DeleteContext(sGlContext);
        SDL_DestroyWindow(sdlWindow);
        sdlWindow = NULL;
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    sGlActive = true;
    InitHdSprites();
    return true;
}

// Where the converted data lives: hd2d/data on PC; on the Switch inside the
// .nro (RomFS, always the right version for the build), else data/ on the SD card
static void DataPath(char *out, size_t size, const char *sub)
{
    const char *dataDir = Harness_DataDir();

    if (dataDir != NULL)
    {
        snprintf(out, size, "%s/%s", dataDir, sub);
    }
    else
    {
#ifdef __SWITCH__
        struct stat st;
        snprintf(out, size, "romfs:/data/%s", sub);
        if (stat(out, &st) != 0)
            snprintf(out, size, "sdmc:/switch/pokeemerald/data/%s", sub);
#else
        char *base = SDL_GetBasePath();
        snprintf(out, size, "%shd2d/data/%s", base != NULL ? base : "./", sub);
        SDL_free(base);
#endif
    }
}

static void InitHdSprites(void)
{
    char dir[1024];

    if (Harness_HdSpritesEnabled())
    {
        DataPath(dir, sizeof(dir), "hdmon");
        if (!HdSprites_Init(dir))
            HdSettings_SetProblem(HD_OPT_HDSPRITES, "Sem dados");
    }
    if (Harness_Field3DEnabled())
    {
        DataPath(dir, sizeof(dir), "field");
        switch (Field3D_Init(dir))
        {
        case FIELD3D_DATA_MISSING:
            HdSettings_SetProblem(HD_OPT_FIELD3D, "Sem dados");
            break;
        case FIELD3D_DATA_OLD:
            HdSettings_SetProblem(HD_OPT_FIELD3D, "Antigos");
            break;
        }
    }
}

static bool CreateSdlRendererOutput(int width, int height, Uint32 windowFlags)
{
    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, windowFlags);
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return false;
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#ifdef __SWITCH__
    // 720 / 160 is not an integer; scale by 4 so every GBA pixel has the same size
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
#endif

    sdlTexture = SDL_CreateTexture(sdlRenderer,
                                   SDL_PIXELFORMAT_ABGR1555,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL)
    {
        DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

static void PresentOutput(void)
{
    if (sGlActive)
    {
        int width, height;
        SDL_GL_GetDrawableSize(sdlWindow, &width, &height);
        GlRenderer_SetComposeScale(GlRenderer_ComposeScaleFor(width, height));
        GlRenderer_Present(width, height);
        SDL_GL_SwapWindow(sdlWindow);
    }
    else
    {
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
        SDL_RenderPresent(sdlRenderer);
    }
}

static void ReadSaveFile(char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL)
    {
        sSaveFile = fopen(path, "w+b");
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL)
    {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
        fflush(sSaveFile); // the Switch only commits file data on flush/close
    }
}

void Platform_StoreSaveFile(void)
{
    StoreSaveFile();
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    DBGPRINTF("ReadFlash(sectorNum=0x%04X,offset=0x%08X,size=0x%02X)\n",sectorNum,offset,size);
    FILE * savefile = fopen(Harness_SavePath(), "r+b");
    if (savefile == NULL)
    {
        puts("Error opening save file.");
        return;
    }
    if (fseek(savefile, (sectorNum << gFlash->sector.shift) + offset, SEEK_SET))
    {
        fclose(savefile);
        return;
    }
    if (fread(dest, 1, size, savefile) != size)
    {
        fclose(savefile);
        return;
    }
    fclose(savefile);
}

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    if (!Harness_IsHeadless())
        SDL_QueueAudio(1, audioBuffer, samplesPerFrame);
}


static void CloseSaveFile()
{
    if (sSaveFile != NULL)
    {
        fclose(sSaveFile);
    }
}

// Key mappings
#define KEY_A_BUTTON      SDLK_z
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_a
#define KEY_R_BUTTON      SDLK_s
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key) \
case KEY_##key:  keys &= ~key; break;

#define HANDLE_KEYDOWN(key) \
case KEY_##key:  keys |= key; break;

static u16 keys;

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            OpenController(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            CloseController(event.cdevice.which);
            break;
        case SDL_KEYUP:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYUP(A_BUTTON)
            HANDLE_KEYUP(B_BUTTON)
            HANDLE_KEYUP(START_BUTTON)
            HANDLE_KEYUP(SELECT_BUTTON)
            HANDLE_KEYUP(L_BUTTON)
            HANDLE_KEYUP(R_BUTTON)
            HANDLE_KEYUP(DPAD_UP)
            HANDLE_KEYUP(DPAD_DOWN)
            HANDLE_KEYUP(DPAD_LEFT)
            HANDLE_KEYUP(DPAD_RIGHT)
            case SDLK_SPACE:
                if (speedUp)
                {
                    speedUp = false;
                    timeScale = 1.0;
                    //SDL_ClearQueuedAudio(1);
                    //SDL_PauseAudio(0);
                }
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYDOWN(A_BUTTON)
            HANDLE_KEYDOWN(B_BUTTON)
            HANDLE_KEYDOWN(START_BUTTON)
            HANDLE_KEYDOWN(SELECT_BUTTON)
            HANDLE_KEYDOWN(L_BUTTON)
            HANDLE_KEYDOWN(R_BUTTON)
            HANDLE_KEYDOWN(DPAD_UP)
            HANDLE_KEYDOWN(DPAD_DOWN)
            HANDLE_KEYDOWN(DPAD_LEFT)
            HANDLE_KEYDOWN(DPAD_RIGHT)
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    DoSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
                }
                break;
            case SDLK_h:
                // HD Pokémon sprites on/off
                HdSettings_Toggle(HD_OPT_HDSPRITES);
                HdSettings_Save();
                break;
            case SDLK_f:
                // 3D field on/off
                HdSettings_Toggle(HD_OPT_FIELD3D);
                HdSettings_Save();
                break;
            case SDLK_SPACE:
                if (!speedUp)
                {
                    speedUp = true;
                    timeScale = 5.0;
                    //SDL_PauseAudio(1);
                }
                break;
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            {
                unsigned int w = event.window.data1;
                unsigned int h = event.window.data2;
                
                videoScale = 0;
                if (w / DISPLAY_WIDTH > videoScale)
                    videoScale = w / DISPLAY_WIDTH;
                if (h / DISPLAY_HEIGHT > videoScale)
                    videoScale = h / DISPLAY_HEIGHT;
                if (videoScale < 1)
                    videoScale = 1;

                videoScaleChanged = true;
            }
            break;
        }
    }
}

#ifdef _WIN32
#define STICK_THRESHOLD 0.5f
u16 GetXInputKeys()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    DWORD dwResult = XInputGetState(0, &state);
    u16 xinputKeys = 0;

    if (dwResult == ERROR_SUCCESS)
    {
        /* A */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) >> 12;
        /* B */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) >> 13;
        /* Start */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) >> 1;
        /* Select */ xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) >> 3;
        /* L */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) << 1;
        /* R */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) >> 1;
        /* Up */     xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) << 6;
        /* Down */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) << 6;
        /* Left */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) << 3;
        /* Right */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) << 1;


        /* Control Stick */
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;

        if (xAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_LEFT;
        if (xAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_RIGHT;
        if (yAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_DOWN;
        if (yAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_UP;


        /* Speedup */
        // Note: 'speedup' variable is only (un)set on keyboard input
        double oldTimeScale = timeScale;
        timeScale = (state.Gamepad.bRightTrigger > 0x80 || speedUp) ? 5.0 : 1.0;

        if (oldTimeScale != timeScale)
        {
            if (timeScale > 1.0)
            {
                SDL_PauseAudio(1);
            }
            else
            {
                SDL_ClearQueuedAudio(1);
                SDL_PauseAudio(0);
            }
        }
    }

    return xinputKeys;
}
#endif // _WIN32

static void OpenController(int deviceIndex)
{
    int i;

    if (!SDL_IsGameController(deviceIndex))
        return;
    for (i = 0; i < MAX_CONTROLLERS; i++)
    {
        if (sControllers[i] == NULL)
        {
            sControllers[i] = SDL_GameControllerOpen(deviceIndex);
            return;
        }
    }
}

static void CloseController(SDL_JoystickID instanceId)
{
    int i;

    for (i = 0; i < MAX_CONTROLLERS; i++)
    {
        if (sControllers[i] != NULL
         && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sControllers[i])) == instanceId)
        {
            SDL_GameControllerClose(sControllers[i]);
            sControllers[i] = NULL;
        }
    }
}

// Buttons follow their labels: Switch A/B are GBA A/B, + is Start, - is Select.
// ZR (right trigger) speeds the game up, like Space on the keyboard.
static u16 ReadControllers(void)
{
    static const struct { SDL_GameControllerButton button; u16 key; } sMap[] = {
        {SDL_CONTROLLER_BUTTON_A, A_BUTTON},
        {SDL_CONTROLLER_BUTTON_B, B_BUTTON},
        {SDL_CONTROLLER_BUTTON_START, START_BUTTON},
        {SDL_CONTROLLER_BUTTON_BACK, SELECT_BUTTON},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, L_BUTTON},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R_BUTTON},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, DPAD_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, DPAD_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, DPAD_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DPAD_RIGHT},
    };
    const Sint16 stickThreshold = 16000;
    u16 padKeys = 0;
    bool speedUpHeld = false;
    int i;
    size_t b;

    for (i = 0; i < MAX_CONTROLLERS; i++)
    {
        SDL_GameController *pad = sControllers[i];
        if (pad == NULL)
            continue;
        for (b = 0; b < sizeof(sMap) / sizeof(sMap[0]); b++)
        {
            if (SDL_GameControllerGetButton(pad, sMap[b].button))
                padKeys |= sMap[b].key;
        }
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) < -stickThreshold) padKeys |= DPAD_LEFT;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) >  stickThreshold) padKeys |= DPAD_RIGHT;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) < -stickThreshold) padKeys |= DPAD_UP;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) >  stickThreshold) padKeys |= DPAD_DOWN;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > stickThreshold)
            speedUpHeld = true;
    }

    if (speedUpHeld != sControllerSpeedUp)
    {
        sControllerSpeedUp = speedUpHeld;
        timeScale = (sControllerSpeedUp || speedUp) ? 5.0 : 1.0;
    }
    return padKeys;
}

u16 Platform_GetKeyInput(void)
{
    u16 live;
#ifdef _WIN32
    u16 gamepadKeys = GetXInputKeys();
    live = ((gamepadKeys != 0) ? gamepadKeys : keys) | ReadControllers();
#else
    live = keys | ReadControllers();
#endif
    // the options menu takes the buttons while it is open
    if (HdMenu_BlocksGameInput())
        live = 0;
    return Harness_GetKeys(live);
}

// Settings live next to the save; headless runs only take them from PE_HD & co.
static void InitSettings(void)
{
    char path[1024];
    const char *save = Harness_SavePath();
    const char *dot = strrchr(save, '.');
    size_t len = dot != NULL ? (size_t)(dot - save) : strlen(save);

    if (Harness_IsHeadless())
    {
        HdSettings_Init(NULL);
        return;
    }
    snprintf(path, sizeof(path), "%.*s_hd.cfg", (int)len, save);
    HdSettings_Init(path);
}

// Buttons for the options menu: Tab / ZL open it, arrows or D-pad move,
// Z / Enter / A change, X / Esc / B close
static uint32_t ReadMenuButtons(void)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    uint32_t held = 0;
    int i;

    if (state != NULL)
    {
        if (state[SDL_SCANCODE_TAB]) held |= HDMENU_OPEN;
        if (state[SDL_SCANCODE_UP]) held |= HDMENU_UP;
        if (state[SDL_SCANCODE_DOWN]) held |= HDMENU_DOWN;
        if (state[SDL_SCANCODE_LEFT]) held |= HDMENU_LEFT;
        if (state[SDL_SCANCODE_RIGHT]) held |= HDMENU_RIGHT;
        if (state[SDL_SCANCODE_Z] || state[SDL_SCANCODE_RETURN]) held |= HDMENU_CONFIRM;
        if (state[SDL_SCANCODE_X] || state[SDL_SCANCODE_ESCAPE] || state[SDL_SCANCODE_BACKSPACE]) held |= HDMENU_BACK;
    }
    for (i = 0; i < MAX_CONTROLLERS; i++)
    {
        SDL_GameController *pad = sControllers[i];
        if (pad == NULL)
            continue;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) held |= HDMENU_OPEN;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) held |= HDMENU_UP;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) held |= HDMENU_DOWN;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) held |= HDMENU_LEFT;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) held |= HDMENU_RIGHT;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) < -16000) held |= HDMENU_UP;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) > 16000) held |= HDMENU_DOWN;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) held |= HDMENU_CONFIRM;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) held |= HDMENU_BACK;
    }
    return held;
}

// Once per pass of the main loop: menu input, frame counter, overlay
static void UpdateMenu(void)
{
    static Uint32 sFpsStart;
    static uint32_t sFpsFrames;
    static float sFps = 60.0f;
    const uint8_t *overlay;
    uint32_t version;
    Uint32 now = SDL_GetTicks();

    if (!sGlActive)
        return;
    if (sFpsStart == 0)
        sFpsStart = now;
    if (now - sFpsStart >= 500)
    {
        sFps = (sRenderedFrames - sFpsFrames) * 1000.0f / (now - sFpsStart);
        sFpsFrames = sRenderedFrames;
        sFpsStart = now;
    }
    HdMenu_Update(ReadMenuButtons(), sFps);
    overlay = HdMenu_Overlay(&version);
    GlRenderer_SetOverlay(overlay, version);
}

// PE_TEST_TOUR: the whole map as the 3D field draws it, <dump dir>/<name>.png
static void SaveOverview(const struct Field3DFrame *field, const char *name)
{
    char path[1024];
    const uint8_t *rgb;
    int width, height;

    if (field == NULL)
    {
        printf("overview: %s has no 3D field\n", name);
        return;
    }
    rgb = GlField3D_RenderOverview(field, TestHooks_OverviewPixelsPerBlock(), &width, &height);
    if (rgb == NULL)
        return;
    snprintf(path, sizeof(path), "%s/%s.png", Harness_DumpDir(), name);
    if (!Harness_WritePngRgb(path, rgb, width, height))
        fprintf(stderr, "overview: failed to write %s\n", path);
}

// Renders the PPU output for the frame the game just finished. Returns the
// image to hand to the harness (NULL when it has no use for one).
static const uint16_t *RenderGameFrame(void)
{
    const uint16_t *image = NULL;

    if (sGlActive)
    {
        static uint8_t skipOam[PPU_OAM_COUNT];
        static struct HdSpriteDraw draws[HD_SPRITE_MAX_DRAWS];
        static struct Field3DFrame field;
        bool verify = Harness_VerifyGl();
        int drawCount = HdSprites_Collect(draws, HD_SPRITE_MAX_DRAWS, skipOam, sDrawnFrames * 1000 / 60);
        bool field3d = Field3D_Prepare(&field, skipOam); // after HdSprites_Collect, which clears skipOam

        sDrawnFrames++;
        sRenderedFrames++;
        if (TestHooks_OverviewWanted() != NULL)
            SaveOverview(field3d ? &field : NULL, TestHooks_OverviewWanted());
        // with the 3D field, sprites blend with the scene on the GPU, not with the 2D map
        DrawFrameEx(verify ? sFrameImage : NULL, &sLayers, (drawCount > 0 || field3d) ? skipOam : NULL,
                    field3d ? PPU_DEFER_SPRITE_BLEND : 0);
        GlRenderer_ComposeFrame(&sLayers, draws, drawCount, field3d ? &field : NULL);
        if (TestHooks_GameShotWanted() != NULL)
        {
            char path[1024];
            int width, height;
            const uint8_t *rgb = GlRenderer_ReadFrameRgb(&width, &height);
            snprintf(path, sizeof(path), "%s/%s_game.png", Harness_DumpDir(), TestHooks_GameShotWanted());
            Harness_WritePngRgb(path, rgb, width, height);
        }
        if (verify)
        {
            GlRenderer_ReadFrame(sGpuImage);
            Harness_ReportMismatch(sFrameImage, sGpuImage);
            image = sGpuImage;
        }
        else if (Harness_WantsImage())
        {
            int width, height;
            const uint8_t *rgb = GlRenderer_ReadFrameRgb(&width, &height);
            Harness_DumpRgb(rgb, width, height); // full compose resolution
        }
    }
    else
    {
        DrawFrame(sFrameImage);
        if (sdlTexture != NULL)
            SDL_UpdateTexture(sdlTexture, NULL, sFrameImage, DISPLAY_WIDTH * sizeof (Uint16));
        image = sFrameImage;
    }
    REG_VCOUNT = 161; // prep for being in VBlank period
    return image;
}

// PE_TEST_MENU="frame:keys": from that frame on, presses one menu button
// every other frame. Keys: O open/close, U up, D down, L left, R right,
// C change, B back, '.' nothing.
static uint32_t HeadlessMenuButtons(uint32_t frame)
{
    const char *script = getenv("PE_TEST_MENU");
    const char *keys;
    unsigned long start;
    uint32_t step;

    if (script == NULL || (keys = strchr(script, ':')) == NULL)
        return 0;
    start = strtoul(script, NULL, 10);
    keys++;
    if (frame < start || (frame - start) % 2 != 0)
        return 0;
    step = (frame - start) / 2;
    if (step >= strlen(keys))
        return 0;
    switch (keys[step])
    {
    case 'O': return HDMENU_OPEN;
    case 'U': return HDMENU_UP;
    case 'D': return HDMENU_DOWN;
    case 'L': return HDMENU_LEFT;
    case 'R': return HDMENU_RIGHT;
    case 'C': return HDMENU_CONFIRM;
    case 'B': return HDMENU_BACK;
    }
    return 0;
}

// No visible window and no audio device: run frames back to back for the
// test harness. The GL compositor runs on an offscreen context.
static void RunHeadless(void)
{
    if (Harness_UseGl())
    {
        if (getenv("SDL_VIDEODRIVER") == NULL)
            SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
        if (SDL_Init(SDL_INIT_VIDEO) < 0 || !CreateGlOutput(DISPLAY_WIDTH, DISPLAY_HEIGHT, SDL_WINDOW_HIDDEN))
        {
            fprintf(stderr, "headless: no OpenGL ES 3.0 context (%s)\n", SDL_GetError());
            exit(1);
        }
        GlRenderer_SetComposeScale(Harness_ComposeScale());
    }

    cgb_audio_init(42048);
    AgbMain();

    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    while (isRunning)
    {
        const uint16_t *image;

        ENTER_VBLANK();
        MainLoop();
        TestHooks_OnFrame(Harness_FrameCount());
        if (sGlActive)
        {
            const uint8_t *overlay;
            uint32_t version;
            HdMenu_Update(HeadlessMenuButtons(Harness_FrameCount()), 60.0f);
            overlay = HdMenu_Overlay(&version);
            GlRenderer_SetOverlay(overlay, version);
        }
        image = RenderGameFrame();
        RunDMAsAndVBlank();
        AudioUpdate();
        if (!Harness_EndFrame(image) || TestHooks_Finished())
            isRunning = false;
    }
    CloseSaveFile();
}

int DoMain(void *data)
{
    AgbMain();
}

void VBlankIntrWait(void)
{
    return;
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do
    {
        out |= (bin % 10) * placeCounter;
        placeCounter *= 16;
    }
    while ((bin /= 10) > 0);

    return out;
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = internalClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    internalClock.status = rtc->status;
}

static void UpdateInternalClock(void)
{
    time_t rawTime = time(NULL);
    struct tm *time = localtime(&rawTime);

    internalClock.year = BinToBcd(time->tm_year - 100);
    internalClock.month = BinToBcd(time->tm_mon + 1);
    internalClock.day = BinToBcd(time->tm_mday);
    internalClock.dayOfWeek = BinToBcd(time->tm_wday);
    internalClock.hour = BinToBcd(time->tm_hour);
    internalClock.minute = BinToBcd(time->tm_min);
    internalClock.second = BinToBcd(time->tm_sec);
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->year = internalClock.year;
    rtc->month = internalClock.month;
    rtc->day = internalClock.day;
    rtc->dayOfWeek = internalClock.dayOfWeek;
    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetDateTime: %d-%02d-%02d %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->year),
                                                         ConvertBcdToBinary(rtc->month),
                                                         ConvertBcdToBinary(rtc->day),
                                                         ConvertBcdToBinary(rtc->hour),
                                                         ConvertBcdToBinary(rtc->minute),
                                                         ConvertBcdToBinary(rtc->second));
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    internalClock.month = rtc->month;
    internalClock.day = rtc->day;
    internalClock.dayOfWeek = rtc->dayOfWeek;
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetTime: %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->hour),
                                        ConvertBcdToBinary(rtc->minute),
                                        ConvertBcdToBinary(rtc->second));
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_SetAlarm(u8 *alarmData)
{
    // TODO
}

void SoftReset(u32 resetFlags)
{
    puts("Soft Reset called. Exiting.");
    exit(0);
}

#endif