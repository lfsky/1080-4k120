// SPDX-License-Identifier: GPL-3.0-only

#include <d3d9.h>
#include <cmath>

#include "water_fix.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../event/frame.hpp"
#include "../event/game_loop.hpp"
#include "../halo_data/shaders/shader_blob.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../halo_data/resolution.hpp"

namespace Chimera {

    extern "C" {
        void water_opacity_psh_const_asm() noexcept;
        void water_reflection_psh_const_asm() noexcept;
        void water_bumpmap_memes_asm() noexcept;
        void water_bumpmap_memes_2_asm() noexcept;
        void adjust_water_render_target_mip_asm() noexcept;
        const void *original_opacity_set_vsh = nullptr;
        const void *original_reflection_set_vsh = nullptr;
        std::uint32_t render_target_create_loop_counter = 0;
    }

    static IDirect3DDevice9 **global_d3d9_device = nullptr;
    static D3DCAPS9 *d3d9_device_caps = nullptr;
    static std::byte *global_render_targets = nullptr;
    static std::byte *bump_vertices = nullptr;

    extern "C" void set_water_shader_const(std::byte *shader, std::uint32_t start_register) noexcept {
        // Not how it works on real hardware NV2A, Collection users seem to have an issue with this.
		// Rounded off is closer than plastic looking dogwater shoved down unsuspecting throats.
		// Formula is to 1080x810ify the increase, more or less accurate across resolutions.
		// To do this, we need to artifically increase the dimensions of xy in our math.
		// Include width, otherwise is automatically determined. Account for aspect ratio.
		// Required, since otherwise at lower than this the divison ends up less than 1.
		// Results in shifting the appearance which happens as the display changes.
		// Fov is also taken to account on those, in which 55.41v used as reference.
		// Specific integer is closely around 3840/810 = x*y = 4.740740740740741
		// Instead of directly putting log2f(4.740740740740741) which also works.
		// 2.481481481481481 × 2.037037037037037  (2x) = 5.054869684499313.
		// 1651 / 1238  = 1.333333333 1650 as close as we can get using rounding.
		// 2.396769969445657 × 1.977881257275902 (1.89x) = 4.740526400568301
		// Let the engine do the math, and change the numbers as desired for whatever.
		// Consider whether or not the engine itself genuinely does this. Convert it.
		// 1294 / 1934 using the largest uniform value for the width so it's covered.
		// 2.654601861427094 × 1.78593588417787 (1.89x) = 4.740948722528016
		// Identify whether which number is the culprit for the shifting lod line.
		// Revert to how we did it modified since we know this will defintively work.
		// 1.832920178482895 × 2.586514625681706 (1.89x) = 4.740874849353132
		// At UHD looks 100% authentic to 1080x810 or real hardware using SD.
		// Lower than 1080x810 reverts to 1 size, will investigate later.
		// log2f((get_resolution().height + 1375) / 2015.f) * log2f((get_resolution().width + 1535) / 2015.f) initial commit
		// log2f((get_resolution().width + 1375) / 2015.f) * log2f((get_resolution().height + 1535) / 2015.f) rev1
		// log2f((get_resolution().width + 1651) / 2291.f) * log2f((get_resolution().height + 1238) / 1718.f) aspect ratio
		// log2f((get_resolution().width + 1294) / 1934.f) * log2f((get_resolution().height + 1454) / 1934.f) rev3
        if(d3d9_device_caps->PixelShaderVersion >= 0xffff0101 && start_register == 1) {

            // Why would you do that. Why would you do that.
            float adjusted_lod_bias = log2f((get_resolution().width + 1377) / 2017.f) * log2f((get_resolution().height + 1537) / 2017.f) - (*reinterpret_cast<float *>(shader + 0xE0));

            // Bump map is on sampler 0.
            DWORD *mip_lod_bias = reinterpret_cast<DWORD *>(&adjusted_lod_bias);
            IDirect3DDevice9_SetSamplerState(*global_d3d9_device, 0, D3DSAMP_MIPMAPLODBIAS, *mip_lod_bias);
        }

        // Fog fix depends on ps_2_0 shaders so check caps for memes.
        if(d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            return;
        }

        // Doing fog in the pixel shader so disable this.
        IDirect3DDevice9_SetRenderState(*global_d3d9_device, D3DRS_FOGENABLE, FALSE);

        // The cea water pixel shaders expect an additional constant to handle the atmospheric fog flag.
        auto *flags = reinterpret_cast<std::uint8_t *>(shader + 0x28);
        float psh_constants[4] = {0};
        psh_constants[0] = (*flags >> 2) & 1 ? 1.0f : 0.0f;
        IDirect3DDevice9_SetPixelShaderConstantF(*global_d3d9_device, start_register, psh_constants, 1);
    }

    extern "C" void make_water_go_brr(int i) noexcept {
        auto water_render_target_texture = reinterpret_cast<IDirect3DTexture9 **>(global_render_targets + 20 * 8 + 16);
        IDirect3DSurface9 *mipmap_surface = NULL;
        IDirect3DTexture9_GetSurfaceLevel(*water_render_target_texture, i, &mipmap_surface);
        IDirect3DDevice9_SetRenderTarget(*global_d3d9_device, 0, mipmap_surface);
        IDirect3DSurface9_Release(mipmap_surface);
    }

    extern "C" void make_water_go_brr_pt2(int mip_levels) noexcept {
        auto water_render_target_texture = reinterpret_cast<IDirect3DTexture9 **>(global_render_targets + 20 * 8 + 16);
        auto vertices = reinterpret_cast<const void *>(bump_vertices);

        for(int i = mip_levels; i < 4; i++) {
            IDirect3DSurface9 *mipmap_surface = NULL;
            IDirect3DTexture9_GetSurfaceLevel(*water_render_target_texture, i, &mipmap_surface);
            IDirect3DDevice9_SetRenderTarget(*global_d3d9_device, 0, mipmap_surface);
            IDirect3DSurface9_Release(mipmap_surface);
            IDirect3DDevice9_DrawPrimitiveUP(*global_d3d9_device, D3DPT_TRIANGLEFAN, 2, vertices, 24);
        }
    }

    void reset_sampler_states_for_mips() noexcept {
        // I guess this needs to be unset
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, 0, D3DSAMP_MIPMAPLODBIAS, 0);
    }

    void set_up_water_fix() noexcept {
        global_d3d9_device = reinterpret_cast<IDirect3DDevice9 **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("model_af_set_sampler_states_sig").data() + 1));
        d3d9_device_caps = reinterpret_cast<D3DCAPS9 *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("d3d9_device_caps_sig").data() + 1));

        // Fix the fog sorting.
        static Hook opacity_set_psh_const, reflection_set_psh_const;

        if(get_chimera().feature_present("client_custom_edition")) {
            auto *opacity_psh = get_chimera().get_signature("water_opacity_set_psh_consts_custom_sig").data();
            auto *reflection_psh = get_chimera().get_signature("water_reflection_set_psh_consts_custom_sig").data();
            write_jmp_call(opacity_psh, opacity_set_psh_const, nullptr, reinterpret_cast<const void *>(water_opacity_psh_const_asm));
            write_jmp_call(reflection_psh, reflection_set_psh_const, nullptr, reinterpret_cast<const void *>(water_reflection_psh_const_asm));
        }
        else if(get_chimera().feature_present("client_retail_demo")) {
            auto *opacity_psh = get_chimera().get_signature("water_opacity_set_psh_consts_retail_sig").data() + 4;
            auto *reflection_psh = get_chimera().get_signature("water_reflection_set_psh_consts_retail_sig").data();
            write_jmp_call(opacity_psh, opacity_set_psh_const, reinterpret_cast<const void *>(water_opacity_psh_const_asm), nullptr);
            write_jmp_call(reflection_psh, reflection_set_psh_const, nullptr, reinterpret_cast<const void *>(water_reflection_psh_const_asm));
        }

        // Make ripple mipmapping work. This is based on Mango's fix from ringoworld.
        global_render_targets = *reinterpret_cast<std::byte **>(get_chimera().get_signature("render_targets_initialize_sig").data() + 1);
        std::byte *water_bump = nullptr;
        auto *water_bump_2 = get_chimera().get_signature("water_bumpmap_draw_2_sig").data() + 6;
        std::byte *water_bump_3 = nullptr;
        static Hook render_target_hook, water_bump_hook, water_bump_hook_2, water_bump_hook_3;

        // Make water render target have 4 levels.
        write_jmp_call(get_chimera().get_signature("render_targets_create_texture_sig").data(), render_target_hook, reinterpret_cast<const void *>(adjust_water_render_target_mip_asm), nullptr);

        // Fixed function check that doesn't even work right. This can go in the bin.
        SigByte mod[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        write_code_s(get_chimera().get_signature("water_bumpmap_update_sig").data(), mod);

        if(get_chimera().feature_present("client_retail_demo")) {
            water_bump = get_chimera().get_signature("water_bumpmap_draw_sig").data();
            water_bump_3 = get_chimera().get_signature("water_reflection_draw_verticies_retail_sig").data();
            bump_vertices = *reinterpret_cast<std::byte **>(get_chimera().get_signature("water_bumpmap_vertices_sig").data() + 3);
        }
        else if(get_chimera().feature_present("client_custom_edition")) {
            water_bump = get_chimera().get_signature("water_bumpmap_draw_ce_sig").data() + 6;
            water_bump_3 = get_chimera().get_signature("water_reflection_draw_verticies_custom_sig").data();
            bump_vertices = *reinterpret_cast<std::byte **>(get_chimera().get_signature("water_bumpmap_vertices_ce_sig").data() + 3);
        }

        write_jmp_call(water_bump, water_bump_hook, reinterpret_cast<const void *>(water_bumpmap_memes_asm), nullptr);
        write_jmp_call(water_bump_2, water_bump_hook_2, reinterpret_cast<const void *>(water_bumpmap_memes_2_asm), nullptr);
        write_jmp_call(water_bump_3, water_bump_hook_3, nullptr, reinterpret_cast<const void *>(reset_sampler_states_for_mips));
    }
}
