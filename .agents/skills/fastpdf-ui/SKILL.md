---
name: fastpdf-ui
description: Create or change FastPDF dialogs and UI controls with the shared installation dialog style, compact spacing, and OS theme colors. Use for UI implementation and visual review in this repository.
---

# FastPDF UI Style

Use the installation dialog as the style reference. Use the compact Password and Go To Page dialogs as the reference for text entry. Apply the same font, colors, button shapes, and spacing to new dialogs.

Paths below are relative to the repository root.

## Use The Shared Code

- Read `src/dialog_style.h` and the relevant parts of `src/dialog_style.cpp` before adding UI styling.
- Read `IDD_INSTALLATION`, `IDD_PASSWORD`, and `IDD_GOTO` in `src/app.rc` for layout examples. Read `src/installation_dialog.cpp` for layouts that change with dialog state.
- Call `ui::style_dialog` during dialog initialization. Keep its footer position in sync with the resource layout.
- Use `ui::position_dialog_control` for runtime positions in dialog units. Update the footer with `ui::dialog_footer` when the layout changes.
- Use `ui::show_prompt` or `ui::message_box` for application messages and confirmations. These functions measure text and use the shared style.
- Use the standard Windows `IFileOpenDialog` for Open. Keep its native font, layout, controls, and drawing. Call `ui::enable_system_dialog_theme` to follow the Windows app theme. Open is an exception to the shared dialog style.
- Use `ui::style_common_dialog` for Print. Preserve its native behavior. Use `ui::refresh_dialog_style` when a styled dialog needs a theme refresh.
- Improve the shared functions when a change applies to several dialogs. Avoid a separate palette or button painter for each dialog.

## Font And Text

- Use Segoe UI, 10 pt, for dialog content and controls.
- Capitalize the first letter of each word in dialog titles, including short words: `Go To Page`, `Open Protected PDF`. Preserve names and acronyms such as `FastPDF` and `PDF`.
- Use sentence case for body text and control labels. Keep labels short and clear.
- Preserve access keys, tab order, initial focus, default buttons, and Escape behavior. Keep No or Cancel as the default when the existing confirmation uses it.

## Compact Layout

The following dimensions are dialog units (DLU), not pixels. Convert them with `MapDialogRect` or the shared positioning function so the layout follows the font and DPI.

| Item | Default |
| --- | --- |
| Simple dialog width | 220 DLU |
| Left and right margins | 12 DLU |
| First content row | 9 DLU from the top |
| Text or checkbox row height | 12 DLU |
| Label-to-field gap | 4 DLU |
| Single-line edit height | 12 DLU |
| Last input-to-footer gap | 8 DLU |
| Footer height | 26 DLU |
| Footer button height | 14 DLU |
| Space above and below footer buttons | 6 DLU |
| Gap between buttons | 6 DLU |
| Standard primary button width | 50 DLU |
| Cancel button width | 44 DLU |

Treat these as layout defaults. Measure longer text and widen controls when needed. Do not clip labels to keep a fixed width. The shared prompt code measures button labels and adds horizontal padding.

Size input fields for their content. The password field uses the available 196 DLU body width. The page-number field uses 60 DLU. Both are one line high; do not restore the oversized 18 DLU edit height.

For a simple input dialog, place the label at y=9 with height 12, the field at y=25 with height 12, and the footer at y=45. Put buttons at y=51. Total client height is 71 DLU.

Keep primary and Cancel buttons together at the right of the footer. Separate the footer from the body with the shared background and thin line. Use the shared rounded button style and accent color for the primary action.

Fit the dialog to its content. Remove empty rows and excess space below controls. Keep enough room for multiline notices, file lists, and document previews. The print preview starts from a 230 by 200 DLU panel; check the final layout because its runtime size can change.

## Theme Behavior

- Get colors from `ui::system_dialog_colors()`. Use its named roles for body, footer, fields, borders, text, muted text, links, and actions.
- Follow the Windows application theme (`AppsUseLightTheme`). A dark reference screenshot does not require a permanently dark dialog.
- Preserve high-contrast support and live theme refresh through the shared style code.
- Check child controls, including edit fields, lists, checkboxes, disabled controls, and scrollbars. Do not leave a child control with colors from the other theme.
- Preserve visible focus, selection, hover, pressed, and disabled states in both themes.

## Verify UI Changes

1. Capture the affected dialogs before the change.
2. Build Release without warnings. `./scripts/build.ps1 -Configuration Release` builds and runs the test suite. Follow the repository instructions if a running FastPDF process blocks the build.
3. Run the relevant UI checks. Existing checks include `deployment`, `ui_toolbar`, `print_preview`, and `file_paths`. Add tests only when they verify a useful behavior or prevent a regression.
4. Inspect the running dialogs in dark and light themes. Check text fit, field heights, gaps, button alignment, keyboard access, and focus. Check the affected layout at more than one display scale when font or DPI sizing changes.
5. Capture the result with the same content and display scale as the before image. Present each pair at the same image scale so size changes remain visible. Include both themes for styling changes.
6. Restore any OS theme setting changed for verification. Do not submit print jobs or install the application to check a dialog.

Save review images under `artifacts/`. That folder is ignored by Git. Keep this skill usable without local screenshots or machine-specific paths.
