#include <QTableWidget>
#include <QKeyEvent>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionRange>

class CCopyTableWidget : public QTableWidget
{
    Q_OBJECT
public:
    CCopyTableWidget(QWidget* parent = nullptr) : QTableWidget(parent) {}

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->matches(QKeySequence::Copy)) {
            copySelectionToClipboard();
        }
        else {
            QTableWidget::keyPressEvent(event);
        }
    }

private:
    void copySelectionToClipboard()
    {
        QString textData;
        QList<QTableWidgetSelectionRange> ranges = selectedRanges();
        if (ranges.isEmpty()) return;

        for (int i = 0; i < ranges.count(); ++i) {
            const QTableWidgetSelectionRange& range = ranges.at(i);
            for (int row = range.topRow(); row <= range.bottomRow(); ++row)
            {
                for (int col = range.leftColumn(); col <= range.rightColumn(); ++col)
                {
                    QTableWidgetItem* item = this->item(row, col);
                    if (item) {
                        textData += item->text();
                    }
                    if (col < range.rightColumn()) {
                        textData += "\t";
                    }
                }
                if (row <= range.bottomRow()) {
                    textData += "\n";
                }
            }
        }
        QApplication::clipboard()->setText(textData);
    }
};