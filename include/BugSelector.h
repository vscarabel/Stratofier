/*
Stratofier Stratux AHRS Display
(c) 2018 Allen K. Lair, Sky Fun
*/

#ifndef __BUGSELECTOR_H__
#define __BUGSELECTOR_H__

#include <QDialog>

#include "ui_BugSelector.h"


class BugSelector : public QDialog, public Ui::BugSelector
{
    Q_OBJECT

public:
	enum BugType
	{
		HeadingBug = 10,
		WindBug    = 20,
        ClearBugs  = 30,
        Airports   = 40,
        Overlays   = 50,
        BaroPress  = 60,
        Calibrate  = 70
	};

    explicit BugSelector( QWidget *pParent );
    ~BugSelector();

private slots:
	void headingSel();
	void windSel();
    void clearBugs();
    void airports();
    void overlays();
    void baroPress();
    void calibrate();
};

#endif // __BUGSELECTOR_H__
