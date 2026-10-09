#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <amp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <iterator>
#include <vector>


namespace {
constexpr int BoardSide = 9;
constexpr std::size_t CellCount = BoardSide * BoardSide;
constexpr std::size_t BatchSize = 32768;
constexpr int StageInitialize = 1;
constexpr int StageExecute = 2;
constexpr int StageRead = 3;

using Board = std::array<std::array<short, BoardSide>, BoardSide>;


struct Puzzle {
    Board clues{};
    // Пустые клетки перечислены с конца, поэтому slot 0 остаётся младшим
    // разрядом исходного последовательного перебора.
    std::vector<short> blankCells;
};


struct AmpGpuResources {
    Concurrency::accelerator device;
    Concurrency::accelerator_view view;
    std::string deviceDescription = "C++ AMP GPU";
    std::unique_ptr<Concurrency::array<int, 1>> blankCells;
    std::unique_ptr<Concurrency::array<int, 1>> initialMasks;
    std::unique_ptr<Concurrency::array<int, 1>> seedDigits;
    std::unique_ptr<Concurrency::array<int, 1>> valid;

    explicit AmpGpuResources(const Puzzle &puzzle, const std::function<void(int)> &reportStage)
        : device(selectGpu())
        , view(device.default_view)
    {
        reportStage(StageInitialize);

        blankCells = makeArray(static_cast<int>(CellCount));
        initialMasks = makeArray(3 * BoardSide);
        seedDigits = makeArray(static_cast<int>(CellCount));
        valid = makeArray(static_cast<int>(BatchSize));

        std::array<int, 3 * BoardSide> masks{};
        for (int row = 0; row < BoardSide; ++row) {
            for (int column = 0; column < BoardSide; ++column) {
                const int value = puzzle.clues[row][column];
                if (value == 0)
                    continue;
                const int bit = 1 << (value - 1);
                masks[row] |= bit;
                masks[BoardSide + column] |= bit;
                const int box = (row / 3) * 3 + column / 3;
                masks[2 * BoardSide + box] |= bit;
            }
        }
        Concurrency::copy(masks.begin(), masks.end(), *initialMasks);
        const auto paddedBlanks = padded(puzzle.blankCells);
        Concurrency::copy(paddedBlanks.begin(), paddedBlanks.end(), *blankCells);
    }

    bool searchBatch(const std::vector<short> &seed, std::vector<unsigned char> &results,
                     const std::function<void(int)> &reportStage)
    {
        std::array<int, CellCount> paddedSeed{};
        std::copy(seed.begin(), seed.end(), paddedSeed.begin());
        Concurrency::copy(paddedSeed.begin(), paddedSeed.end(), *seedDigits);

        const int emptyCount = static_cast<int>(seed.size());
        std::size_t batchCount = 1;
        for (std::size_t slot = 0; slot < seed.size() && batchCount < BatchSize; ++slot) {
            if (batchCount > BatchSize / 9) {
                batchCount = BatchSize;
                break;
            }
            batchCount *= 9;
        }
        batchCount = std::min(batchCount, BatchSize);
        const int candidateCount = static_cast<int>(batchCount);
        const int globalSize = static_cast<int>(((batchCount + 31) / 32) * 32);

        Concurrency::array_view<const int, 1> blankCell(*blankCells);
        Concurrency::array_view<const int, 1> initialMask(*initialMasks);
        Concurrency::array_view<const int, 1> seedView(*seedDigits);
        Concurrency::array_view<int, 1> validView(*valid);

        reportStage(StageExecute);
        Concurrency::parallel_for_each(
            view, Concurrency::extent<1>(globalSize),
            [=](Concurrency::index<1> index) restrict(amp) {
                const int candidate = index[0];
                if (candidate >= candidateCount)
                    return;

                int rowMasks[BoardSide];
                int columnMasks[BoardSide];
                int boxMasks[BoardSide];
                for (int unit = 0; unit < BoardSide; ++unit) {
                    rowMasks[unit] = initialMask[unit];
                    columnMasks[unit] = initialMask[BoardSide + unit];
                    boxMasks[unit] = initialMask[2 * BoardSide + unit];
                }

                int offset = candidate;
                int carry = 0;
                for (int slot = 0; slot < emptyCount; ++slot) {
                    const int sum = seedView[slot] + offset % 9 + carry;
                    offset /= 9;
                    carry = sum / 9;
                    const int cell = blankCell[slot];
                    const int row = cell / BoardSide;
                    const int column = cell % BoardSide;
                    const int box = (row / 3) * 3 + column / 3;
                    const int bit = 1 << (sum % 9);
                    if ((rowMasks[row] & bit) || (columnMasks[column] & bit)
                        || (boxMasks[box] & bit)) {
                        validView[candidate] = 0;
                        return;
                    }
                    rowMasks[row] |= bit;
                    columnMasks[column] |= bit;
                    boxMasks[box] |= bit;
                }
                if (offset != 0 || carry != 0) {
                    validView[candidate] = 0;
                    return;
                }

                validView[candidate] = 1;
            });

        std::vector<int> output(BatchSize);
        reportStage(StageRead);
        Concurrency::copy(*valid, output.begin());
        results.resize(batchCount);
        for (std::size_t i = 0; i < batchCount; ++i)
            results[i] = static_cast<unsigned char>(output[i] != 0);
        return std::find(results.begin(), results.end(), static_cast<unsigned char>(1))
               != results.end();
    }

private:
    static Concurrency::accelerator selectGpu()
    {
        const std::vector<Concurrency::accelerator> devices =
            Concurrency::accelerator::get_all();
        for (const Concurrency::accelerator &candidate : devices) {
            if (!candidate.is_emulated
                && candidate.device_path != Concurrency::accelerator::direct3d_ref
                && candidate.device_path != Concurrency::accelerator::direct3d_warp
                && candidate.device_path != Concurrency::accelerator::cpu_accelerator)
                return candidate;
        }
        throw std::runtime_error("No hardware GPU compatible with C++ AMP was found.");
    }

    std::unique_ptr<Concurrency::array<int, 1>> makeArray(int size)
    {
        return std::make_unique<Concurrency::array<int, 1>>(
            Concurrency::extent<1>(size), view);
    }

    static std::array<int, CellCount> padded(const std::vector<short> &values)
    {
        std::array<int, CellCount> result{};
        std::copy(values.begin(), values.end(), result.begin());
        return result;
    }
};

bool cluesAreConsistent(const Puzzle &puzzle)
{
    bool rowSeen[BoardSide][BoardSide + 1]{};
    bool columnSeen[BoardSide][BoardSide + 1]{};
    bool boxSeen[BoardSide][BoardSide + 1]{};
    for (int row = 0; row < BoardSide; ++row) {
        for (int column = 0; column < BoardSide; ++column) {
            const short value = puzzle.clues[row][column];
            if (value < 0 || value > BoardSide)
                return false;
            if (value == 0)
                continue;
            const int box = (row / 3) * 3 + column / 3;
            if (rowSeen[row][value] || columnSeen[column][value] || boxSeen[box][value])
                return false;
            rowSeen[row][value] = true;
            columnSeen[column][value] = true;
            boxSeen[box][value] = true;
        }
    }
    return true;
}

bool advanceSeed(std::vector<short> &seed, std::uint64_t amount)
{
    std::uint64_t carry = amount;
    for (short &digit : seed) {
        const int sum = digit + static_cast<int>(carry % BoardSide);
        carry /= BoardSide;
        digit = static_cast<short>(sum % BoardSide);
        carry += static_cast<std::uint64_t>(sum / 9);
        if (carry == 0)
            return true;
    }
    return carry == 0;
}

Board makeCandidate(const Puzzle &puzzle, const std::vector<short> &seed, std::size_t offset)
{
    Board candidate = puzzle.clues;
    std::uint64_t remainder = offset;
    int carry = 0;
    for (std::size_t slot = 0; slot < seed.size(); ++slot) {
        const int sum = seed[slot] + static_cast<int>(remainder % BoardSide) + carry;
        remainder /= BoardSide;
        carry = sum / 9;
        const int cell = puzzle.blankCells[slot];
        candidate[cell / BoardSide][cell % BoardSide] = static_cast<short>(sum % 9 + 1);
    }
    return candidate;
}

std::string solve(const Puzzle &puzzle, Board &solution,
                  const std::atomic_bool &cancelled,
                  const std::function<void(int)> &reportStage)
{
    if (puzzle.blankCells.empty()) {
        solution = puzzle.clues;
        return "solution found: the supplied grid is already complete";
    }
    AmpGpuResources gpu(puzzle, reportStage);
    std::vector<short> seed(puzzle.blankCells.size(), 0);
    std::vector<unsigned char> results;
    std::uint64_t batches = 0;
    for (;;) {
        if (cancelled.load())
            return "search cancelled";
        if (gpu.searchBatch(seed, results, reportStage)) {
            const auto found = std::find(results.begin(), results.end(),
                                         static_cast<unsigned char>(1));
            const std::size_t offset = static_cast<std::size_t>(found - results.begin());
            solution = makeCandidate(puzzle, seed, offset);
            return "solution found on " + gpu.deviceDescription + " after checking GPU batch "
                   + std::to_string(batches + 1)
                   + ", candidate offset " + std::to_string(offset);
        }

        ++batches;
        if (!advanceSeed(seed, results.size()))
            return "no solution: the complete base-9 search space was exhausted";
    }
}

} // namespace

namespace {
constexpr UINT ResetButtonId = 1000;
constexpr UINT SolveButtonId = 1001;
constexpr UINT LoadButtonId = 1002;
constexpr UINT SaveButtonId = 1003;
constexpr UINT SolveFinishedMessage = WM_APP + 1;
constexpr UINT SolveProgressMessage = WM_APP + 2;

struct SolveResult {
    std::string message;
    Board board{};
    bool hasSolution = false;
};

struct WindowState {
    HWND window = nullptr;
    HWND cells[CellCount]{};
    HWND buttons[4]{};
    HWND status = nullptr;
    HFONT font = nullptr;
    Board initialBoard{};
    std::atomic_bool cancelled = false;
    std::thread solverThread;
    std::mutex resultMutex;
    std::optional<SolveResult> pendingResult;
};

std::wstring widen(const std::string &text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1)
        return L"";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    result.pop_back();
    return result;
}

void setStatus(WindowState &state, const std::wstring &text)
{
    SetWindowTextW(state.status, text.c_str());
}

void setBoard(WindowState &state, const Board &board)
{
    for (int row = 0; row < BoardSide; ++row) {
        for (int column = 0; column < BoardSide; ++column) {
            const std::wstring value = board[row][column] == 0
                                           ? L""
                                           : std::to_wstring(board[row][column]);
            SetWindowTextW(state.cells[row * BoardSide + column], value.c_str());
        }
    }
}

Board getBoard(const WindowState &state)
{
    Board board{};
    for (int row = 0; row < BoardSide; ++row) {
        for (int column = 0; column < BoardSide; ++column) {
            wchar_t value[8]{};
            GetWindowTextW(state.cells[row * BoardSide + column], value,
                           static_cast<int>(std::size(value)));
            board[row][column] = value[0] == L'\0' ? 0 : static_cast<short>(value[0] - L'0');
        }
    }
    return board;
}

Puzzle makePuzzle(const Board &board)
{
    Puzzle puzzle;
    puzzle.clues = board;
    for (int cell = static_cast<int>(CellCount) - 1; cell >= 0; --cell) {
        const int row = cell / BoardSide;
        const int column = cell % BoardSide;
        if (board[row][column] != 0)
            continue;
        puzzle.blankCells.push_back(static_cast<short>(cell));
    }
    return puzzle;
}

void enableControls(WindowState &state, bool enabled)
{
    for (HWND cell : state.cells)
        EnableWindow(cell, enabled);
    for (HWND button : state.buttons)
        EnableWindow(button, enabled);
}

void startSearch(WindowState &state)
{
    const auto board = getBoard(state);
    Puzzle puzzle = makePuzzle(board);
    if (!cluesAreConsistent(puzzle)) {
        setStatus(state, L"В исходных цифрах есть конфликт по строке, столбцу или блоку.");
        return;
    }

    if (state.solverThread.joinable())
        state.solverThread.join();
    state.cancelled = false;
    enableControls(state, false);
    setStatus(state, L"Идёт перебор кандидатов на GPU; пустых клеток: "
                       + std::to_wstring(puzzle.blankCells.size()) + L".");
    state.solverThread = std::thread([&state, puzzle] {
        SolveResult result;
        try {
            const auto reportStage = [&state](int stage) {
                PostMessageW(state.window, SolveProgressMessage, static_cast<WPARAM>(stage), 0);
            };
            result.message = solve(puzzle, result.board, state.cancelled, reportStage);
            result.hasSolution = result.message.rfind("solution found", 0) == 0;
        } catch (const std::exception &error) {
            result.message = std::string("error: ") + error.what();
        }
        {
            std::lock_guard<std::mutex> lock(state.resultMutex);
            state.pendingResult = std::move(result);
        }
        PostMessageW(state.window, SolveFinishedMessage, 0, 0);
    });
}

bool chooseFile(WindowState &state, bool save, std::wstring &path)
{
    wchar_t fileName[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = L"Sudoku text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (save) {
        dialog.Flags |= OFN_OVERWRITEPROMPT;
        dialog.lpstrTitle = L"Сохранить судоку";
        if (!GetSaveFileNameW(&dialog))
            return false;
    } else {
        dialog.Flags |= OFN_FILEMUSTEXIST;
        dialog.lpstrTitle = L"Загрузить судоку";
        if (!GetOpenFileNameW(&dialog))
            return false;
    }
    path = fileName;
    return true;
}

bool loadBoardFile(const std::wstring &path, Board &board)
{
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input)
        return false;

    std::vector<short> digits;
    char character = 0;
    while (input.get(character)) {
        if (character >= '0' && character <= '9')
            digits.push_back(static_cast<short>(character - '0'));
        else if (character == '.')
            digits.push_back(0);
        else if (!std::isspace(static_cast<unsigned char>(character))
                 && character != ',' && character != '|' && character != '-')
            return false;
        if (digits.size() > CellCount)
            return false;
    }
    if (digits.size() != CellCount)
        return false;
    for (int row = 0; row < BoardSide; ++row) {
        for (int column = 0; column < BoardSide; ++column)
            board[row][column] = digits[row * BoardSide + column];
    }
    return true;
}

bool saveBoardFile(const std::wstring &path, const Board &board)
{
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    if (!output)
        return false;
    for (int row = 0; row < BoardSide; ++row) {
        for (int column = 0; column < BoardSide; ++column)
            output << board[row][column];
        if (row != BoardSide - 1)
            output << '\n';
    }
    return static_cast<bool>(output);
}

void createControls(WindowState &state, HINSTANCE instance)
{
    state.font = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    constexpr int cellSize = 54;
    for (int row = 0; row < 9; ++row) {
        for (int column = 0; column < 9; ++column) {
            const int x = 75 + column * 57 + (column / 3) * 4;
            const int y = 42 + row * 57 + (row / 3) * 4;
            const int cell = row * 9 + column;
            state.cells[cell] = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_NUMBER,
                x, y, cellSize, cellSize, state.window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(100 + cell)), instance, nullptr);
            SendMessageW(state.cells[cell], WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
            SendMessageW(state.cells[cell], EM_SETLIMITTEXT, 1, 0);
        }
    }

    const wchar_t *labels[] = {L"Сбросить на начальную позицию", L"Найти решение",
                               L"Загрузить из файла", L"Сохранить в файл"};
    const UINT ids[] = {ResetButtonId, SolveButtonId, LoadButtonId, SaveButtonId};
    for (int i = 0; i < 4; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const int x = 75 + column * 270;
        const int y = 604 + row * 46;
        state.buttons[i] = CreateWindowExW(
            0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, y, 260, 38, state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])),
            instance, nullptr);
        SendMessageW(state.buttons[i], WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    }

    state.status = CreateWindowExW(0, L"STATIC", L"Введите цифры 1–9; пустые клетки оставьте пустыми.",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   75, 570, 530, 24, state.window,
                                   reinterpret_cast<HMENU>(1004), instance, nullptr);
    SendMessageW(state.status, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        state = static_cast<WindowState *>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (!state)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        createControls(*state, reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ResetButtonId:
            setBoard(*state, state->initialBoard);
            setStatus(*state, L"Позиция восстановлена.");
            return 0;
        case SolveButtonId:
            startSearch(*state);
            return 0;
        case LoadButtonId: {
            std::wstring path;
            if (!chooseFile(*state, false, path))
                return 0;
            Board board{};
            if (!loadBoardFile(path, board)) {
                setStatus(*state, L"Не удалось прочитать файл: нужно ровно 81 цифру 0–9 или точку.");
                return 0;
            }
            state->initialBoard = board;
            setBoard(*state, board);
            setStatus(*state, L"Судоку загружена; сброс вернёт к этой позиции.");
            return 0;
        }
        case SaveButtonId: {
            std::wstring path;
            if (!chooseFile(*state, true, path))
                return 0;
            if (!saveBoardFile(path, getBoard(*state)))
                setStatus(*state, L"Не удалось сохранить файл.");
            else
                setStatus(*state, L"Судоку сохранена.");
            return 0;
        }
        }
        break;
    case SolveFinishedMessage: {
        if (state->solverThread.joinable())
            state->solverThread.join();
        std::optional<SolveResult> result;
        {
            std::lock_guard<std::mutex> lock(state->resultMutex);
            result = std::move(state->pendingResult);
            state->pendingResult.reset();
        }
        enableControls(*state, true);
        if (!result)
            return 0;
        if (result->hasSolution) {
            setBoard(*state, result->board);
            setStatus(*state, L"Решение найдено. Кнопка сброса вернёт исходную позицию.");
        } else if (result->message.rfind("no solution", 0) == 0) {
            setStatus(*state, L"Решение не найдено: проверено всё пространство перебора.");
        } else if (result->message.rfind("error:", 0) == 0) {
            setStatus(*state, widen(result->message));
        } else {
            setStatus(*state, L"Перебор остановлен.");
        }
        return 0;
    }
    case SolveProgressMessage:
        switch (wParam) {
        case StageInitialize:
            setStatus(*state, L"Подключение к GPU через C++ AMP…");
            break;
        case StageExecute:
            setStatus(*state, L"GPU перебирает очередной пакет кандидатов…");
            break;
        case StageRead:
            setStatus(*state, L"Получение результатов пакета с GPU…");
            break;
        }
        return 0;
    case WM_CLOSE:
        state->cancelled = true;
        if (state->solverThread.joinable())
            state->solverThread.join();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state->font)
            DeleteObject(state->font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const wchar_t className[] = L"DumbSudokuWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&windowClass))
        return 1;

    WindowState state;
    RECT rectangle{0, 0, 660, 700};
    AdjustWindowRect(&rectangle, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND window = CreateWindowExW(
        0, className, L"Dumb Sudoku — перебор на GPU",
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        nullptr, nullptr, instance, &state);
    if (!window)
        return 1;

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
