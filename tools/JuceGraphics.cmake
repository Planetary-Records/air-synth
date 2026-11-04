# Keep the pinned checkout immutable. Only the graphics compilation source uses this build-private copy.
function(racksynth_prepare_juce_graphics source_modules output_modules)
    set(native_file "juce_graphics/native/juce_Direct2DGraphicsContextImpl_windows.cpp")
    file(READ "${source_modules}/${native_file}" original)
    string(REPLACE "\r\n" "\n" original "${original}")
    string(SHA256 preimage "${original}")
    if(NOT preimage STREQUAL "8b50ef7068b2491083d9f6a1c09a4fad0e71efeba8d0f5e0c4bb0a988fa1e8fe")
        message(FATAL_ERROR "Unsupported JUCE graphics layer preimage: ${preimage}")
    endif()
    string(REPLACE "        D2D1_LAYER_PARAMETERS1 params;" "        D2D1_LAYER_PARAMETERS1 params;\n        mutable ComSmartPtr<ID2D1Layer> resource;" patched "${original}")
    string(REPLACE "                context->PushLayer (layer->params, nullptr);" [=[            {
                // Automatic layer sharing made later image draws depend on prior compositions.
                // Keep this resource alive until its matching PopLayer, with the same layer parameters.
                if (layer->resource == nullptr)
                {
                    const auto hr = context->CreateLayer (nullptr, layer->resource.resetAndGetPointerAddress());
                    jassertquiet (SUCCEEDED (hr));
                }
                context->PushLayer (layer->params, layer->resource);
            }]=] patched "${patched}")
    set(png_file "juce_graphics/image_formats/juce_PNGLoader.cpp")
    file(READ "${source_modules}/${png_file}" png_original)
    string(REPLACE "\r\n" "\n" png_original "${png_original}")
    string(SHA256 png_preimage "${png_original}")
    if(NOT png_preimage STREQUAL "aaaed3498767fd3e42f88d6932870f12e314e46c75143da69087e833f7723d0b")
        message(FATAL_ERROR "Unsupported JUCE PNG loader preimage: ${png_preimage}")
    endif()
    # The caller installed its live error buffer. The callback must unwind to it, not save its own expired frame.
    string(REPLACE [=[       #ifdef PNG_SETJMP_SUPPORTED
        setjmp (png_jmpbuf (p));
       #else
        longjmp (*(jmp_buf*) p->error_ptr, 1);
       #endif]=] [=[        longjmp (*static_cast<jmp_buf*> (png_get_error_ptr (p)), 1);]=] png_patched "${png_original}")
    file(COPY "${source_modules}/juce_graphics" DESTINATION "${output_modules}"
        PATTERN "juce_Direct2DGraphicsContextImpl_windows.cpp" EXCLUDE
        PATTERN "juce_PNGLoader.cpp" EXCLUDE)
    # Unlike file(WRITE), CONFIGURE leaves the timestamp alone when reconfiguring identical input.
    file(CONFIGURE OUTPUT "${output_modules}/${native_file}" CONTENT "${patched}" @ONLY NEWLINE_STYLE UNIX)
    file(CONFIGURE OUTPUT "${output_modules}/${png_file}" CONTENT "${png_patched}" @ONLY NEWLINE_STYLE UNIX)
endfunction()
