#pragma once
#include <QColor>
#include <QString>
#include <QRectF>
#include "AnnotationTypes.h"

struct MarkupUndoEntry {
    enum Kind { AddShape, DeleteShape, MoveAnnot, RetextAnnot, RestyleAnnot, AddNote, DeleteNote, ContentsEdit };
    Kind          kind = AddShape;
    int           page = -1;
    QString       uid;
    AnnotSnapshot snap;

    // MoveAnnot
    double dxU = 0.0, dyU = 0.0;

    // RetextAnnot
    QString oldText, newText;

    // RestyleAnnot
    QColor oldColor, newColor;
    float  oldWidth = 0.0f,   newWidth = 0.0f;
    bool   oldFill  = false,  newFill  = false;
    int    oldFillAlpha = 255, newFillAlpha = 255;
    float  oldFontSize = 0.0f, newFontSize = 0.0f;
    bool   isFreeText = false;

    // AddNote / DeleteNote
    QRectF  noteRect;
    QString noteText, noteAuthor;
    QColor  noteColor;
    float   noteFontSize = 11.0f;
    bool    noteWithBackground = false;
    bool    noteIsPopup = false;
};
