// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d9.h>
namespace wxl_quest_marker {
        class SavedMarkerStates
        {
        public:
            DWORD zEnable = TRUE, zWrite = FALSE, alphaBlend = FALSE;
            DWORD srcBlend = 0, dstBlend = 0, alphaTest = FALSE;
            DWORD cull = D3DCULL_CCW, lighting = TRUE;
            DWORD colorWrite = 0x0F, fog = FALSE, stencil = FALSE;
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9* ps = nullptr;
            IDirect3DDevice9* dev = nullptr;
            bool captured = false;
            // Transforms
            D3DMATRIX matWorld, matView, matProj;
            // Texture stage states for all 8 stages
            DWORD tssColorOp[8], tssColorArg1[8], tssColorArg2[8];
            DWORD tssAlphaOp[8], tssAlphaArg1[8], tssAlphaArg2[8];
            // Sampler states for stage 0
            DWORD sampMipFilter, sampMinFilter, sampMagFilter;
            // ZFUNC, texture 0
            DWORD zFunc;
            IDirect3DBaseTexture9* tex0 = nullptr;
            // Stream source, FVF, alpha func/ref
            IDirect3DVertexBuffer9* streamVB = nullptr;
            UINT streamOffset = 0;
            UINT streamStride = 0;
            IDirect3DVertexDeclaration9* declaration = nullptr;
            DWORD alphaFunc = 0;
            DWORD alphaRef = 0;
            DWORD blendOp = 0;
            DWORD srgbWrite = 0;

            SavedMarkerStates() = default;
            SavedMarkerStates(const SavedMarkerStates&) = delete;
            SavedMarkerStates& operator=(const SavedMarkerStates&) = delete;
            void Capture(IDirect3DDevice9* d)
            {
                dev = d;
                d->GetRenderState(D3DRS_ZENABLE, &zEnable);
                d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
                d->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
                d->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
                d->GetRenderState(D3DRS_DESTBLEND, &dstBlend);
                d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
                d->GetRenderState(D3DRS_CULLMODE, &cull);
                d->GetRenderState(D3DRS_LIGHTING, &lighting);
                d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
                d->GetRenderState(D3DRS_FOGENABLE, &fog);
                d->GetRenderState(D3DRS_STENCILENABLE, &stencil);
                // D3D9 Get* interface methods already return an AddRef'd pointer. Adding another
                // reference here leaked the currently bound shaders once per rendered marker frame.
                // More importantly, doing the same for a bound DEFAULT-pool texture below kept that
                // resource alive after OnDeviceLost, so Reset could never leave
                // D3DERR_DEVICENOTRESET after minimizing/restoring the client.
                d->GetVertexShader(&vs);
                d->GetPixelShader(&ps);
                // Transforms
                d->GetTransform(D3DTS_WORLD, &matWorld);
                d->GetTransform(D3DTS_VIEW, &matView);
                d->GetTransform(D3DTS_PROJECTION, &matProj);
                // Texture stage states for all 8 stages
                for (DWORD s = 0; s < 8; ++s) {
                    d->GetTextureStageState(s, D3DTSS_COLOROP,   &tssColorOp[s]);
                    d->GetTextureStageState(s, D3DTSS_COLORARG1, &tssColorArg1[s]);
                    d->GetTextureStageState(s, D3DTSS_COLORARG2, &tssColorArg2[s]);
                    d->GetTextureStageState(s, D3DTSS_ALPHAOP,   &tssAlphaOp[s]);
                    d->GetTextureStageState(s, D3DTSS_ALPHAARG1, &tssAlphaArg1[s]);
                    d->GetTextureStageState(s, D3DTSS_ALPHAARG2, &tssAlphaArg2[s]);
                }
                // Sampler states for stage 0
                d->GetSamplerState(0, D3DSAMP_MIPFILTER, &sampMipFilter);
                d->GetSamplerState(0, D3DSAMP_MINFILTER, &sampMinFilter);
                d->GetSamplerState(0, D3DSAMP_MAGFILTER, &sampMagFilter);
                // ZFUNC, texture 0
                d->GetRenderState(D3DRS_ZFUNC, &zFunc);
                d->GetTexture(0, &tex0);
                // Stream source, FVF, alpha func/ref
                d->GetStreamSource(0, &streamVB, &streamOffset, &streamStride);
                d->GetVertexDeclaration(&declaration);
                d->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunc);
                d->GetRenderState(D3DRS_ALPHAREF, &alphaRef);
                d->GetRenderState(D3DRS_BLENDOP, &blendOp);
                { union { DWORD d; float f; } u; d->GetRenderState(D3DRS_SRGBWRITEENABLE, &u.d); srgbWrite = u.d; }
                captured = true;
            }

            void Restore()
            {
                if (!dev || !captured) return;
                captured = false;
                dev->SetRenderState(D3DRS_ZENABLE, zEnable);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alphaBlend);
                dev->SetRenderState(D3DRS_SRCBLEND, srcBlend);
                dev->SetRenderState(D3DRS_DESTBLEND, dstBlend);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTest);
                dev->SetRenderState(D3DRS_CULLMODE, cull);
                dev->SetRenderState(D3DRS_LIGHTING, lighting);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
                dev->SetRenderState(D3DRS_FOGENABLE, fog);
                dev->SetRenderState(D3DRS_STENCILENABLE, stencil);
                dev->SetVertexShader(vs);
                dev->SetPixelShader(ps);
                if (vs) vs->Release();
                if (ps) ps->Release();
                vs = nullptr; ps = nullptr;
                // Transforms
                dev->SetTransform(D3DTS_WORLD, &matWorld);
                dev->SetTransform(D3DTS_VIEW, &matView);
                dev->SetTransform(D3DTS_PROJECTION, &matProj);
                // Texture stage states
                for (DWORD s = 0; s < 8; ++s) {
                    dev->SetTextureStageState(s, D3DTSS_COLOROP,   tssColorOp[s]);
                    dev->SetTextureStageState(s, D3DTSS_COLORARG1, tssColorArg1[s]);
                    dev->SetTextureStageState(s, D3DTSS_COLORARG2, tssColorArg2[s]);
                    dev->SetTextureStageState(s, D3DTSS_ALPHAOP,   tssAlphaOp[s]);
                    dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, tssAlphaArg1[s]);
                    dev->SetTextureStageState(s, D3DTSS_ALPHAARG2, tssAlphaArg2[s]);
                }
                // Sampler states
                dev->SetSamplerState(0, D3DSAMP_MIPFILTER, sampMipFilter);
                dev->SetSamplerState(0, D3DSAMP_MINFILTER, sampMinFilter);
                dev->SetSamplerState(0, D3DSAMP_MAGFILTER, sampMagFilter);
                // ZFUNC, texture 0
                dev->SetRenderState(D3DRS_ZFUNC, zFunc);
                dev->SetTexture(0, tex0);
                if (tex0) { tex0->Release(); tex0 = nullptr; }
                // Stream source, FVF, alpha func/ref
                dev->SetStreamSource(0, streamVB, streamOffset, streamStride);
                if (streamVB) streamVB->Release();
                // A programmable declaration has FVF=0; SetFVF(0) cannot restore it.
                dev->SetVertexDeclaration(declaration);
                if (declaration) declaration->Release();
                declaration = nullptr;
                dev->SetRenderState(D3DRS_ALPHAFUNC, alphaFunc);
                dev->SetRenderState(D3DRS_ALPHAREF, alphaRef);
                dev->SetRenderState(D3DRS_BLENDOP, blendOp);
                dev->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
            }

            ~SavedMarkerStates() { Restore(); }
        };
}
