#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────────────────────────
// Structures
// ─────────────────────────────────────────────

struct ROI {
    std::string label;
    int x, y, w, h;
};

// ─────────────────────────────────────────────
// Save / Load ROIs
// ─────────────────────────────────────────────

void saveROIs(const std::vector<ROI>& rois, const std::string& path) {
    json j;
    for (const auto& roi : rois) {
        j["rois"][roi.label] = {
            {"x", roi.x}, {"y", roi.y},
            {"w", roi.w}, {"h", roi.h}
        };
    }
    std::ofstream f(path);
    f << j.dump(4);
    std::cout << "[Saved] ROIs written to " << path << "\n";
}

std::vector<ROI> loadROIs(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[Error] Cannot open ROI file: " << path << "\n";
        return {};
    }
    json j;
    f >> j;

    std::vector<ROI> rois;
    for (auto& [label, coords] : j["rois"].items()) {
        ROI roi;
        roi.label = label;
        roi.x = coords["x"];
        roi.y = coords["y"];
        roi.w = coords["w"];
        roi.h = coords["h"];
        rois.push_back(roi);
    }
    return rois;
}

// ─────────────────────────────────────────────
// ROI Selection Mode
// ─────────────────────────────────────────────

void selectROIs(const std::string& imagePath, const std::string& outputJson) {
    cv::Mat img = cv::imread(imagePath);
    if (img.empty()) {
        std::cerr << "[Error] Could not load image: " << imagePath << "\n";
        return;
    }

    std::vector<ROI> rois;
    bool selecting = true;

    std::cout << "\n[ROI Selection Mode]\n";
    std::cout << "  Draw a box with your mouse, then press SPACE or ENTER to confirm.\n";
    std::cout << "  After each selection you'll be prompted for a label.\n";
    std::cout << "  Press 'q' when done.\n\n";

    while (selecting) {
        cv::Rect selection = cv::selectROI("Select ROI (SPACE/ENTER to confirm, 'q' to quit)", img, false, false);

        // Check if user closed the window or pressed 'q'
        int key = cv::waitKey(0);
        if (key == 'q' || key == 27 || (selection.width == 0 && selection.height == 0)) {
            selecting = false;
            break;
        }

        if (selection.width > 0 && selection.height > 0) {
            std::string label;
            std::cout << "Enter label for this ROI: ";
            std::cin >> label;

            // Check for duplicate labels
            for (const auto& r : rois) {
                if (r.label == label) {
                    std::cout << "[Warning] Label '" << label << "' already exists. Overwriting.\n";
                }
            }

            ROI roi;
            roi.label = label;
            roi.x = selection.x;
            roi.y = selection.y;
            roi.w = selection.width;
            roi.h = selection.height;
            rois.push_back(roi);

            // Draw confirmed ROI on image for reference
            cv::rectangle(img, selection, cv::Scalar(0, 255, 0), 2);
            cv::putText(img, label, cv::Point(selection.x, selection.y - 5),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

            std::cout << "[Added] '" << label << "' at ("
                      << roi.x << ", " << roi.y << ", "
                      << roi.w << ", " << roi.h << ")\n\n";
        }
    }

    cv::destroyAllWindows();

    if (!rois.empty()) {
        saveROIs(rois, outputJson);
    } else {
        std::cout << "[Info] No ROIs were selected.\n";
    }
}

// ─────────────────────────────────────────────
// OCR on a single image using loaded ROIs
// ─────────────────────────────────────────────

json runOCR(const cv::Mat& img, const std::vector<ROI>& rois) {
    tesseract::TessBaseAPI tess;
    if (tess.Init(nullptr, "eng", tesseract::OEM_LSTM_ONLY)) {
        std::cerr << "[Error] Could not initialize Tesseract.\n";
        return {};
    }
    // Optimise for digits; change to PSM_SINGLE_LINE for mixed text
    tess.SetPageSegMode(tesseract::PSM_SINGLE_LINE);
    tess.SetVariable("tessedit_char_whitelist", "0123456789.-+");

    json results;
    for (const auto& roi : rois) {
        // Bounds check
        cv::Rect rect(roi.x, roi.y, roi.w, roi.h);
        rect &= cv::Rect(0, 0, img.cols, img.rows);
        if (rect.empty()) {
            results[roi.label] = "";
            continue;
        }

        cv::Mat crop = img(rect).clone();

        // Preprocessing: grayscale → threshold
        cv::Mat gray, thresh;
        cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
        cv::threshold(gray, thresh, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

        tess.SetImage(thresh.data, thresh.cols, thresh.rows, 1, thresh.step);
        char* text = tess.GetUTF8Text();
        std::string result(text);
        delete[] text;

        // Trim whitespace / newlines
        result.erase(result.find_last_not_of(" \t\r\n") + 1);
        result.erase(0, result.find_first_not_of(" \t\r\n"));

        results[roi.label] = result;
        std::cout << "  [OCR] " << roi.label << " -> \"" << result << "\"\n";
    }

    tess.End();
    return results;
}

// ─────────────────────────────────────────────
// Batch Processing Mode
// ─────────────────────────────────────────────

void batchProcess(const std::string& inputDir,
                  const std::string& roiJson,
                  const std::string& outputDir) {
    std::vector<ROI> rois = loadROIs(roiJson);
    if (rois.empty()) {
        std::cerr << "[Error] No ROIs loaded. Aborting batch.\n";
        return;
    }

    fs::create_directories(outputDir);

    std::vector<std::string> extensions = {".png", ".jpg", ".jpeg", ".bmp", ".tiff", ".tif"};
    int processed = 0;

    std::cout << "\n[Batch Processing] Scanning: " << inputDir << "\n\n";

    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        // Lowercase extension comparison
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool valid = false;
        for (const auto& e : extensions) if (ext == e) { valid = true; break; }
        if (!valid) continue;

        std::string imgPath = entry.path().string();
        std::string stem    = entry.path().stem().string();

        std::cout << "Processing: " << stem << "\n";
        cv::Mat img = cv::imread(imgPath);
        if (img.empty()) {
            std::cerr << "  [Warning] Could not load image, skipping.\n";
            continue;
        }

        json result;
        result["image"]   = entry.path().filename().string();
        result["results"] = runOCR(img, rois);

        std::string outPath = outputDir + "/" + stem + "_results.json";
        std::ofstream f(outPath);
        f << result.dump(4);
        std::cout << "  [Saved] " << outPath << "\n\n";
        ++processed;
    }

    std::cout << "[Done] Processed " << processed << " image(s).\n";
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

void printUsage(const char* prog) {
    std::cout << "\nUsage:\n";
    std::cout << "  Select ROIs:     " << prog << " select <image> [rois.json]\n";
    std::cout << "  Batch OCR:       " << prog << " batch  <input_dir> <rois.json> [output_dir]\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << prog << " select photo.png rois.json\n";
    std::cout << "  " << prog << " batch ./images rois.json ./results\n\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "select") {
        std::string imagePath  = argv[2];
        std::string outputJson = (argc >= 4) ? argv[3] : "rois.json";
        selectROIs(imagePath, outputJson);

    } else if (mode == "batch") {
        if (argc < 4) {
            std::cerr << "[Error] batch mode requires <input_dir> and <rois.json>\n";
            printUsage(argv[0]);
            return 1;
        }
        std::string inputDir  = argv[2];
        std::string roiJson   = argv[3];
        std::string outputDir = (argc >= 5) ? argv[4] : "results";
        batchProcess(inputDir, roiJson, outputDir);

    } else {
        std::cerr << "[Error] Unknown mode: " << mode << "\n";
        printUsage(argv[0]);
        return 1;
    }

    return 0;
}
