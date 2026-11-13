/** 
 * @file  PropTextColors.h
 *
 * @brief Declaration file for PropTextColors propertyheet
 *
 */
#pragma once

#include "ColorButton.h"
#include "OptionsPanel.h"
#include "BlameGutter.h"
#include <array>

class COptionsMgr;
class SyntaxColors;

/** @brief Property page for colors options; used in options property sheet */
class PropTextColors : public OptionsPanel
{

// Construction
public:

	PropTextColors(COptionsMgr *optionsMgr, SyntaxColors *pColors);

// Implement IOptionsPanel
	virtual void ReadOptions() override;
	virtual void WriteOptions() override;

// Dialog Data
private:

	SyntaxColors *m_pTempColors;
	std::array<COLORREF, 16> m_cCustColors;

	//{{AFX_DATA(PropTextColors)
	enum { IDD = IDD_PROPPAGE_COLORS_TEXT };
	CColorButton    m_btnWhitespaceBackground;
	CColorButton    m_btnRegularBackground;
	CColorButton    m_btnRegularText;
	CColorButton    m_btnSelectionBackground;
	CColorButton    m_btnSelectionText;
	CColorButton    m_btnMarginBackground;
	/** @brief Accent colours the blame gutter uses per author. */
	CColorButton    m_btnBlameTint[BlameTint::COUNT];
	bool            m_bCustomColors;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(PropTextColors)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:

	typedef enum { SET_DEFAULTS, READ_OPTIONS, LOAD_COLORS } OPERATION;

	void BrowseColorAndSave(CColorButton & colorButton, int colorIndex);
	/** @brief Pick a colour for one blame gutter slot; saved on OK. */
	void BrowseBlameTintColor(int index);
	void SerializeColorsToFromScreen(OPERATION op);
	void SerializeColorToFromScreen(OPERATION op, CColorButton & btn, int colorIndex);
	void EnableColorButtons(bool bEnable);

	// Generated message map functions
	//{{AFX_MSG(PropTextColors)
	afx_msg void OnWhitespaceBackgroundColor();
	afx_msg void OnRegularBackgroundColor();
	afx_msg void OnRegularTextColor();
	afx_msg void OnSelectionBackgroundColor();
	afx_msg void OnSelectionTextColor();
	afx_msg void OnMarginBackgroundColor();
	afx_msg void OnBlameTintColor(UINT nID);
	afx_msg void OnDefaultsStandardColors();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};
