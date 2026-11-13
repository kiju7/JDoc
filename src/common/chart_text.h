#pragma once
// The words of a DrawingML chart (c:chartSpace): what a reader sees on it
// besides the plotted values. Hancom keeps its charts in this form too — a
// Chart/chartN.xml part in HWPX, an OOXMLChartContents stream in the OLE
// object of an HWP chart.
// License: MIT

#include <cstddef>
#include <string>

namespace jdoc {

// Chart title, axis titles, then for each series its name and its category
// labels, space-separated; "" when the XML is not a chart or holds no text.
std::string drawingml_chart_text(const char* xml, size_t size);

} // namespace jdoc
