OPTION CASEMAP:NONE
ag2 SEGMENT ALIGN(4) READ WRITE ALIAS(".idata$2") 'DATA'
PUBLIC __IMPORT_DESCRIPTOR_airack_saturation_release
__IMPORT_DESCRIPTOR_airack_saturation_release LABEL DWORD
    DD IMAGEREL ag_lookup
    DD 0, 0
    DD IMAGEREL ag_dll
    DD IMAGEREL __imp_airack_saturation_release_pow
ag2 ENDS
ag3 SEGMENT ALIGN(4) READ WRITE ALIAS(".idata$3") 'DATA'
ag_null_descriptor DD 5 DUP(0)
ag3 ENDS
ag4 SEGMENT ALIGN(8) READ WRITE ALIAS(".idata$4") 'DATA'
ag_lookup LABEL QWORD
    DD IMAGEREL ag_hint
    DD 0
    DQ 0
ag4 ENDS
ag5 SEGMENT ALIGN(8) READ WRITE ALIAS(".idata$5") 'DATA'
PUBLIC __imp_airack_saturation_release_pow
__imp_airack_saturation_release_pow LABEL QWORD
    DD IMAGEREL ag_hint
    DD 0
    DQ 0
ag5 ENDS
ag6 SEGMENT ALIGN(2) READ WRITE ALIAS(".idata$6") 'DATA'
ag_hint DW 0
    DB "pow", 0
    ALIGN 2
ag_dll DB "api-ms-win-crt-math-l1-1-0.dll", 0
ag6 ENDS
END
