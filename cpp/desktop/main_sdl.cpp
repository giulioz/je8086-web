#include "../core/jp8000_emulator.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

std::vector<uint8_t> loadFile(const char* filename) {
    std::vector<uint8_t> data;
    FILE* file = std::fopen(filename, "rb");
    if (!file) {
        return data;
    }

    std::fseek(file, 0, SEEK_END);
    long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (length > 0) {
        data.resize(static_cast<size_t>(length));
        std::fread(data.data(), 1, static_cast<size_t>(length), file);
    }

    std::fclose(file);
    return data;
}

SwitchType mapKey(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_1: return kSwitch_1;
        case SDL_SCANCODE_2: return kSwitch_2;
        case SDL_SCANCODE_3: return kSwitch_3;
        case SDL_SCANCODE_4: return kSwitch_4;
        case SDL_SCANCODE_5: return kSwitch_5;
        case SDL_SCANCODE_6: return kSwitch_6;
        case SDL_SCANCODE_7: return kSwitch_7;
        case SDL_SCANCODE_8: return kSwitch_8;
        case SDL_SCANCODE_Q: return kSwitch_ValueDown;
        case SDL_SCANCODE_W: return kSwitch_ValueUp;
        case SDL_SCANCODE_E: return kSwitch_PerformSel;
        case SDL_SCANCODE_U: return kSwitch_Exit;
        case SDL_SCANCODE_I: return kSwitch_Write;
        case SDL_SCANCODE_D: return kSwitch_Rec;
        default: return static_cast<SwitchType>(0);
    }
}

class DesktopHost {
public:
    DesktopHost(std::vector<uint8_t> rom, std::vector<uint8_t> ram) {
        emulator_ = new JP8000Emulator(std::move(rom), std::move(ram), [this](int32_t l, int32_t r) { PostSample(l, r); });
    }

    bool setup(int pageSize = 512, int pageNum = 8) {
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
            std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
            return false;
        }

        window_ = SDL_CreateWindow(
            "JP-8000",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            JP8000Emulator::lcdWidth(),
            JP8000Emulator::lcdHeight(),
            SDL_WINDOW_SHOWN
        );
        if (!window_) {
            std::fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
            return false;
        }

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            std::fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
            return false;
        }

        texture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            JP8000Emulator::lcdWidth(),
            JP8000Emulator::lcdHeight()
        );
        if (!texture_) {
            std::fprintf(stderr, "Texture creation failed: %s\n", SDL_GetError());
            return false;
        }

        int audio_page_size = (pageSize / 2) * 2; // must be even
		audio_buffer_size = audio_page_size * pageNum * 2;
        sample_buffer = (short*)calloc(audio_buffer_size, sizeof(short));
        if (!sample_buffer)
			printf("Cannot allocate audio buffer.\n");

        SDL_AudioSpec desired {};
        desired.freq = 88200;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        desired.callback = &DesktopHost::audioCallback;
        desired.userdata = this;

        SDL_AudioSpec obtained {};
        audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (!audioDevice_) {
            std::fprintf(stderr, "Audio device open failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(audioDevice_, 0);

        return true;
    }

    void run() {
        SDL_Thread *thread = SDL_CreateThread(work_thread, "work thread", this);

        bool running = true;
        std::vector<uint32_t> pixelStaging(JP8000Emulator::lcdWidth() * JP8000Emulator::lcdHeight());

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    running = false;
                }

                if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                    if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        running = false;
                        continue;
                    }

                    const SwitchType which = mapKey(event.key.keysym.scancode);
                    if (which != 0) {
                        SDL_LockAudioDevice(audioDevice_);
                        if (event.type == SDL_KEYDOWN) {
                            emulator_->pressButton(which);
                            if (which == kSwitch_Rec) emulator_->pressButton(kSwitch_Hold);
                        } else {
                            emulator_->releaseButton(which);
                            if (which == kSwitch_Rec) emulator_->releaseButton(kSwitch_Hold);
                        }
                        SDL_UnlockAudioDevice(audioDevice_);
                    }
                }
            }

            SDL_LockAudioDevice(audioDevice_);
            emulator_->renderLcd();
            const auto& pixels = emulator_->lcdPixels();
            std::copy(pixels.begin(), pixels.end(), pixelStaging.begin());
            SDL_UnlockAudioDevice(audioDevice_);

            SDL_UpdateTexture(texture_, nullptr, pixelStaging.data(), JP8000Emulator::lcdWidth() * static_cast<int>(sizeof(uint32_t)));
            SDL_RenderClear(renderer_);
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
            SDL_RenderPresent(renderer_);
            SDL_Delay(15);
        }

        if (audioDevice_) {
            SDL_LockAudioDevice(audioDevice_);
            shuttingDown_.store(true, std::memory_order_release);
            SDL_UnlockAudioDevice(audioDevice_);
            SDL_PauseAudioDevice(audioDevice_, 1);
            SDL_CloseAudioDevice(audioDevice_);
            audioDevice_ = 0;
        }
        SDL_WaitThread(thread, 0);
        SDL_CloseAudio();
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        if (sample_buffer) free(sample_buffer);
        if (emulator_) {delete emulator_; emulator_ = nullptr;}
        SDL_Quit();
    }

private:
    static void audioCallback(void* userdata, uint8_t* stream, int len) {
        auto* self = static_cast<DesktopHost*>(userdata);
        len /= 2;
		memcpy(stream, &self->sample_buffer[self->sample_read_ptr], len * 2);
		memset(&self->sample_buffer[self->sample_read_ptr], 0, len * 2);
		self->sample_read_ptr += len;
		self->sample_read_ptr %= self->audio_buffer_size;
    }

    static int SDLCALL work_thread(void* data) {
        DesktopHost *host = (DesktopHost*)data;
        host->workthread();
        return 0;
    }
	
    void workthread() {
        SDL_mutex *work_thread_lock = SDL_CreateMutex();
		
		SDL_LockMutex(work_thread_lock);
		while (!shuttingDown_.load(std::memory_order_acquire))
		{
			if (sample_read_ptr == sample_write_ptr)
			{
				SDL_UnlockMutex(work_thread_lock);
				while (sample_read_ptr == sample_write_ptr) SDL_Delay(1);
				SDL_LockMutex(work_thread_lock);
			}
			
			emulator_->step();
		}
		SDL_UnlockMutex(work_thread_lock);
		SDL_DestroyMutex(work_thread_lock);
    }

    void PostSample(int32_t sampleL, int32_t sampleR)
	{
		sampleL >>= 8;
		sampleR >>= 8;
		sample_buffer[sample_write_ptr + 0] = sampleL;
		sample_buffer[sample_write_ptr + 1] = sampleR;
		sample_write_ptr = (sample_write_ptr + 2) % audio_buffer_size;
	}

    JP8000Emulator* emulator_;
    SDL_Window* window_ {};
    SDL_Renderer* renderer_ {};
    SDL_Texture* texture_ {};
    SDL_AudioDeviceID audioDevice_ {};
    std::atomic<bool> shuttingDown_ {false};
    int audio_buffer_size, sample_read_ptr {0}, sample_write_ptr {0};
    short *sample_buffer;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <rom-file> [ram-dump-file]\n", argv[0]);
        return 1;
    }

    std::vector<uint8_t> rom = loadFile(argv[1]);
    if (rom.empty()) {
        std::fprintf(stderr, "Failed to read ROM file: %s\n", argv[1]);
        return 1;
    }

    std::vector<uint8_t> ram;
    if (argc >= 3) {
        ram = loadFile(argv[2]);
    }

    try {
        DesktopHost host(rom, ram);
        if (!host.setup()) {
            return 1;
        }
        host.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Fatal error: %s\n", ex.what());
        return 1;
    }

    return 0;
}
