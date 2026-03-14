#include <codecvt>
#include <cstring>
#include <locale>
#include <string>

#include <psp2/apputil.h>
#include <psp2/ime_dialog.h>
#include <vitaGL.h>

static uint16_t dialog_res_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
#define GUI_MAX_TEXT_SIZE 128

void gui_init_ime(void)
{
	SceAppUtilInitParam init_params = {};
	SceAppUtilBootParam init_boot_params = {};
	sceAppUtilInit(&init_params, &init_boot_params);
	SceCommonDialogConfigParam cmnDlgCfgParam;
	sceCommonDialogConfigParamInit(&cmnDlgCfgParam);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cmnDlgCfgParam.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cmnDlgCfgParam.enterButtonAssign);
	sceCommonDialogSetConfigParam(&cmnDlgCfgParam);
}

char *gui_open_text_dialog(std::string title, std::string initial_text)
{
    std::u16string title_utf16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(title.data());
    std::u16string initial_text_utf16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(initial_text.data());

    SceImeDialogParam dialog_param;
    sceImeDialogParamInit(&dialog_param);
    dialog_param.type = SCE_IME_TYPE_BASIC_LATIN;
    dialog_param.option = 0;
    dialog_param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    dialog_param.title = (const SceWChar16*)title_utf16.c_str();
    dialog_param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;

    sceClibMemset(dialog_res_text, 0, sizeof(dialog_res_text));
    sceClibMemcpy(dialog_res_text, initial_text_utf16.c_str(), initial_text_utf16.length() * 2);
    dialog_param.initialText = dialog_res_text;
    dialog_param.inputTextBuffer = dialog_res_text;

    sceImeDialogInit(&dialog_param);

    // Wait for user to input data and close common dialog
    while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        vglSwapBuffers(GL_TRUE);
    }

    SceImeDialogResult result = {};
    sceImeDialogGetResult(&result);
    char *text = (char *)malloc(GUI_MAX_TEXT_SIZE);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        // Converting text from UTF16 to UTF8
        std::u16string utf16_str = (char16_t*)dialog_res_text;
        std::string utf8_str = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(utf16_str.data());
        
        if (text)
            strncpy(text, utf8_str.c_str(), GUI_MAX_TEXT_SIZE);
    }

    sceImeDialogTerm();
    return text;
}
