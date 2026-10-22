"""A small document for search navigation and visual checks."""
from pathlib import Path
from fixtures import pdf, stream


def search_document(path):
    pages = [
        ("Coverage and duration", [
            ("The warranty covers defects in materials and workmanship.", 652),
            ("A warranty claim must include the serial number.", 610),
            ("Warranty terms apply from the date of delivery.", 568),
            ("Keep the receipt for your warranty claim.", 138),
        ]),
        ("Service procedure", [
            ("To request warranty service, contact the supplier.", 652),
            ("Approved warranty repairs include parts and labour.", 610),
            ("Each warrantyholder must keep proof of purchase.", 568),
        ]),
        ("Limits and exclusions", [
            ("The warranty does not cover incorrect installation.", 652),
            ("For more information, refer to the service guide.", 610),
        ]),
    ]
    objects = [b"<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R >>",
               b"<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>",
               b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"]
    for index, (title, lines) in enumerate(pages):
        content = f"BT /F1 10 Tf 0.4 g 1 0 0 1 54 740 Tm (PRODUCT GUIDE / SERVICE INFORMATION) Tj ET\n"
        content += f"BT /F1 24 Tf 0 g 1 0 0 1 54 694 Tm ({title}) Tj ET\n"
        for text, y in lines:
            content += f"BT /F1 14 Tf 0 g 1 0 0 1 54 {y} Tm ({text}) Tj ET\n"
        content += f"0.8 G 54 80 m 558 80 l S BT /F1 10 Tf 0.4 g 1 0 0 1 54 60 Tm (Product guide - {index + 1}) Tj ET\n"
        objects += [f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents {5 + index * 2} 0 R >>".encode(), stream(content)]
    objects += [
        b"<< /Type /Outlines /First 11 0 R /Last 14 0 R /Count 3 >>",
        b"<< /Parent 10 0 R /Title (Coverage and duration) /Dest [4 0 R /Fit] /Next 13 0 R /First 12 0 R /Last 12 0 R /Count -1 >>",
        b"<< /Parent 11 0 R /Title (Warranty terms) /Dest [4 0 R /Fit] >>",
        b"<< /Parent 10 0 R /Title (Service procedure) /Dest [6 0 R /Fit] /Prev 11 0 R /Next 14 0 R >>",
        b"<< /Parent 10 0 R /Title (Limits and exclusions) /Dest [8 0 R /Fit] /Prev 13 0 R >>",
    ]
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(pdf(objects))


if __name__ == "__main__":
    import sys
    search_document(sys.argv[1])
