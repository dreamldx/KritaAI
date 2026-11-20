/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include <KoColorSpaceRegistry.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_selection.h>
#include <kis_undo_stores.h>
#include <kistest.h>

#include "../ai/tools/LayerOps.h"

using namespace AiChat;

namespace {

/// A real image of Krita with a real undo history. Layers, bottom to top: Sky, Hills, Trees; and an empty group "Box" on top.
struct Fixture {
    Fixture()
    {
        store = new KisSurrogateUndoStore();
        image = new KisImage(store, 100, 80, KoColorSpaceRegistry::instance()->rgb8(), "test");
        for (const char *name : {"Sky", "Hills", "Trees"}) {
            KisPaintLayerSP layer = new KisPaintLayer(image.data(), name, OPACITY_OPAQUE_U8, image->colorSpace());
            image->addNode(layer, image->rootLayer(), image->rootLayer()->lastChild());
        }
        KisNodeSP group = new KisGroupLayer(image.data(), "Box", OPACITY_OPAQUE_U8);
        image->addNode(group, image->rootLayer(), image->rootLayer()->lastChild());
        image->waitForDone();
    }

    /// The layers of the top level, top first.
    QStringList names(const KisNodeSP &parent = KisNodeSP()) const
    {
        KisNodeSP root = parent ? parent : KisNodeSP(image->rootLayer());
        QStringList result;
        for (KisNodeSP node = root->lastChild(); node; node = node->prevSibling()) {
            result << node->name();
        }
        return result;
    }

    KisNodeSP layer(const QString &name) const
    {
        QString error;
        return LayerOps(image).resolve(name, &error);
    }

    bool run(const LayerCommand &command, QString *summary = nullptr, QString *error = nullptr)
    {
        QString s;
        QString e;
        const bool done = LayerOps(image).apply(command, &s, &e);
        if (summary) {
            *summary = s;
        }
        if (error) {
            *error = e;
        }
        return done;
    }

    /// Edit > Undo
    void undo()
    {
        image->waitForDone();
        store->undo();
        image->waitForDone();
    }

    KisSurrogateUndoStore *store {nullptr};
    KisImageSP image;
};

LayerCommand command(LayerCommand::Type type, const QString &layer = QString(), const QString &text = QString())
{
    LayerCommand result;
    result.type = type;
    result.layer = layer;
    result.text = text;
    return result;
}

} // namespace

class LayerOpsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLayersAreFoundByIdAndByName()
    {
        Fixture f;
        KisNodeSP hills = f.layer("Hills");
        QVERIFY(hills);
        QString error;
        LayerOps ops(f.image);
        QCOMPARE(ops.resolve(hills->uuid().toString(QUuid::WithoutBraces), &error), hills);
        QCOMPARE(ops.resolve("hills", &error), hills); // not picky about the case

        QVERIFY(!ops.resolve("Mountains", &error));
        QVERIFY(error.contains("Mountains"));
        QVERIFY(!ops.resolve("  ", &error));
        QVERIFY(!ops.resolve("root", &error)); // the invisible top of the tree is not a layer

        // two layers of the same name: the id tells them apart
        QVERIFY(f.run(command(LayerCommand::Duplicate, "Hills")));
        QVERIFY(f.run(command(LayerCommand::Rename, "Hills (copy)", "Hills"))); // the copy is named like the original
        QVERIFY(!ops.resolve("Hills", &error));
        QVERIFY(error.contains("id"));
    }

    void testAddLayer()
    {
        Fixture f;
        QString summary;
        QVERIFY(f.run(command(LayerCommand::AddLayer, QString(), "Notes"), &summary));
        QCOMPARE(f.names().first(), QString("Notes")); // on top
        QVERIFY(summary.contains("Notes"));
        QVERIFY(summary.contains("id")); // the model needs it for the next call

        // above a given layer
        QVERIFY(f.run(command(LayerCommand::AddLayer, "Sky", "Under")));
        QCOMPARE(f.names(), (QStringList {"Notes", "Box", "Trees", "Hills", "Under", "Sky"}));

        // without name: a free one
        QVERIFY(f.run(command(LayerCommand::AddLayer)));
        QVERIFY(!f.names().first().isEmpty());

        // one step of the history
        f.undo();
        QCOMPARE(f.names().size(), 7 - 1);
        f.undo();
        QVERIFY(!f.names().contains("Under"));
    }

    void testAddGroupAndMoveIntoIt()
    {
        Fixture f;
        QVERIFY(f.run(command(LayerCommand::AddGroup, QString(), "Scenery")));
        KisNodeSP group = f.layer("Scenery");
        QVERIFY(group);
        QVERIFY(dynamic_cast<KisGroupLayer *>(group.data()));

        QString error;
        QVERIFY(f.run(command(LayerCommand::MoveToGroup, "Hills", "Scenery"), nullptr, &error));
        QCOMPARE(f.layer("Hills")->parent(), group);
        QCOMPARE(f.names(group), QStringList {"Hills"});

        // and out again
        QVERIFY(f.run(command(LayerCommand::MoveToGroup, "Hills", QString())));
        QCOMPARE(f.layer("Hills")->parent(), KisNodeSP(f.image->rootLayer()));

        // errors: not a group, already there, a group into itself
        QVERIFY(!f.run(command(LayerCommand::MoveToGroup, "Hills", "Sky"), nullptr, &error));
        QVERIFY(error.contains("not a group"));
        QVERIFY(!f.run(command(LayerCommand::MoveToGroup, "Hills", QString()), nullptr, &error));
        QVERIFY(error.contains("already"));
        QVERIFY(f.run(command(LayerCommand::AddGroup, QString(), "Inner")));
        QVERIFY(f.run(command(LayerCommand::MoveToGroup, "Inner", "Scenery")));
        QVERIFY(!f.run(command(LayerCommand::MoveToGroup, "Scenery", "Inner"), nullptr, &error));
        QVERIFY(error.contains("itself"));
    }

    void testAddImageLayerPutsThePictureWhereAsked()
    {
        Fixture f;
        QImage picture(30, 20, QImage::Format_ARGB32);
        picture.fill(QColor(255, 0, 0, 255));

        LayerCommand add = command(LayerCommand::AddImageLayer, QString(), "Result");
        add.image = picture;
        add.point = QPoint(10, 15);
        QString summary;
        QVERIFY(f.run(add, &summary));

        KisNodeSP layer = f.layer("Result");
        QVERIFY(layer);
        QCOMPARE(f.names().first(), QString("Result")); // on top
        QCOMPARE(layer->paintDevice()->exactBounds(), QRect(10, 15, 30, 20));
        QColor pixel;
        layer->paintDevice()->pixel(20, 20, &pixel);
        QCOMPARE(pixel.red(), 255);
        QCOMPARE(pixel.alpha(), 255);

        f.undo();
        QVERIFY(!f.names().contains("Result"));

        LayerCommand none = command(LayerCommand::AddImageLayer);
        QString error;
        QVERIFY(!f.run(none, nullptr, &error));
    }

    void testRenameVisibilityOpacityAndBlendMode()
    {
        Fixture f;
        QString error;

        QVERIFY(f.run(command(LayerCommand::Rename, "Trees", "Forest")));
        QCOMPARE(f.names().at(1), QString("Forest"));
        f.undo();
        QCOMPARE(f.names().at(1), QString("Trees"));
        QVERIFY(!f.run(command(LayerCommand::Rename, "Trees", "  "), nullptr, &error));

        KisNodeSP trees = f.layer("Trees");
        LayerCommand hide = command(LayerCommand::SetVisible, "Trees");
        hide.flag = false;
        QVERIFY(f.run(hide));
        QVERIFY(!trees->visible());
        f.undo();
        QVERIFY(trees->visible());

        LayerCommand opacity = command(LayerCommand::SetOpacity, "Trees");
        opacity.number = 50;
        QVERIFY(f.run(opacity));
        QCOMPARE(int(trees->opacity()), 128);
        f.undo();
        QCOMPARE(int(trees->opacity()), int(OPACITY_OPAQUE_U8));
        opacity.number = 150;
        QVERIFY(!f.run(opacity, nullptr, &error));
        QVERIFY(error.contains("100"));

        QVERIFY(f.run(command(LayerCommand::SetBlendMode, "Trees", "multiply")));
        QCOMPARE(trees->compositeOpId(), QString("multiply"));
        f.undo();
        QCOMPARE(trees->compositeOpId(), QString("normal"));
        QVERIFY(!f.run(command(LayerCommand::SetBlendMode, "Trees", "sparkle"), nullptr, &error));
        QVERIFY(error.contains("sparkle"));
        QCOMPARE(trees->compositeOpId(), QString("normal")); // nothing happened
    }

    void testMoveContent()
    {
        Fixture f;
        QImage picture(10, 10, QImage::Format_ARGB32);
        picture.fill(Qt::blue);
        LayerCommand add = command(LayerCommand::AddImageLayer, QString(), "Dot");
        add.image = picture;
        add.point = QPoint(5, 5);
        QVERIFY(f.run(add));
        KisNodeSP dot = f.layer("Dot");

        LayerCommand move = command(LayerCommand::MoveContent, "Dot");
        move.point = QPoint(20, -3);
        QString error;
        QVERIFY(f.run(move, nullptr, &error));
        QCOMPARE(dot->exactBounds(), QRect(25, 2, 10, 10));
        f.undo();
        QCOMPARE(dot->exactBounds(), QRect(5, 5, 10, 10));

        move.point = QPoint();
        QVERIFY(!f.run(move, nullptr, &error));
        move.layer = "Box";
        move.point = QPoint(1, 1);
        QVERIFY(!f.run(move, nullptr, &error));
        QVERIFY(error.contains("group"));
    }

    void testDuplicateAndDelete()
    {
        Fixture f;
        KisNodeSP hills = f.layer("Hills");
        QString summary;
        QVERIFY(f.run(command(LayerCommand::Duplicate, "Hills"), &summary));
        QCOMPARE(f.names(), (QStringList {"Box", "Trees", "Hills (copy)", "Hills", "Sky"})); // the copy is above the original
        KisNodeSP copy = f.layer("Hills (copy)");
        QVERIFY(copy);
        QVERIFY(copy != hills);
        QVERIFY(copy->uuid() != hills->uuid());
        f.undo();
        QCOMPARE(f.names(), (QStringList {"Box", "Trees", "Hills", "Sky"}));

        QVERIFY(f.run(command(LayerCommand::Delete, "Hills"), &summary));
        QVERIFY(summary.contains("Undo"));
        QCOMPARE(f.names(), (QStringList {"Box", "Trees", "Sky"}));
        f.undo();
        QCOMPARE(f.names(), (QStringList {"Box", "Trees", "Hills", "Sky"}));

        // a locked layer is not thrown away
        f.layer("Sky")->setUserLocked(true);
        QString error;
        QVERIFY(!f.run(command(LayerCommand::Delete, "Sky"), nullptr, &error));
        QVERIFY(error.contains("locked"));
        QVERIFY(f.names().contains("Sky"));
    }

    void testReorder()
    {
        Fixture f; // top first: Box, Trees, Hills, Sky
        QString error;

        QVERIFY(f.run(command(LayerCommand::Reorder, "Sky", "top")));
        QCOMPARE(f.names(), (QStringList {"Sky", "Box", "Trees", "Hills"}));
        f.undo();
        QCOMPARE(f.names(), (QStringList {"Box", "Trees", "Hills", "Sky"}));

        QVERIFY(f.run(command(LayerCommand::Reorder, "Box", "bottom")));
        QCOMPARE(f.names(), (QStringList {"Trees", "Hills", "Sky", "Box"}));
        f.undo();

        QVERIFY(f.run(command(LayerCommand::Reorder, "Hills", "up")));
        QCOMPARE(f.names(), (QStringList {"Box", "Hills", "Trees", "Sky"}));
        f.undo();

        QVERIFY(f.run(command(LayerCommand::Reorder, "Hills", "down")));
        QCOMPARE(f.names(), (QStringList {"Box", "Trees", "Sky", "Hills"}));
        f.undo();

        // nothing to do, or nonsense
        QVERIFY(!f.run(command(LayerCommand::Reorder, "Box", "top"), nullptr, &error));
        QVERIFY(error.contains("already"));
        QVERIFY(!f.run(command(LayerCommand::Reorder, "Box", "up"), nullptr, &error));
        QVERIFY(!f.run(command(LayerCommand::Reorder, "Sky", "down"), nullptr, &error));
        QVERIFY(!f.run(command(LayerCommand::Reorder, "Sky", "sideways"), nullptr, &error));
        QVERIFY(error.contains("sideways"));
    }

    void testScaleImage()
    {
        Fixture f;
        QImage picture(100, 80, QImage::Format_ARGB32);
        picture.fill(Qt::green);
        LayerCommand add = command(LayerCommand::AddImageLayer, QString(), "Full");
        add.image = picture;
        QVERIFY(f.run(add));

        LayerCommand scale = command(LayerCommand::ScaleImage);
        scale.size = QSize(300, 240);
        QString summary;
        QString error;
        QVERIFY2(f.run(scale, &summary, &error), qPrintable(error));
        QCOMPARE(f.image->width(), 300);
        QCOMPARE(f.image->height(), 240);
        QCOMPARE(f.layer("Full")->exactBounds(), QRect(0, 0, 300, 240)); // the layers went along
        QVERIFY(summary.contains("300 x 240"));

        f.undo();
        QCOMPARE(f.image->width(), 100);
        QCOMPARE(f.image->height(), 80);
        QCOMPARE(f.layer("Full")->exactBounds(), QRect(0, 0, 100, 80));

        scale.size = QSize(100, 80);
        QVERIFY(!f.run(scale, nullptr, &error)); // same size
        scale.size = QSize(0, 10);
        QVERIFY(!f.run(scale, nullptr, &error));
        scale.size = QSize(50000, 50000); // 2.5 billion pixels
        QVERIFY(!f.run(scale, nullptr, &error));
        QVERIFY(error.contains("not possible"));
        QCOMPARE(f.image->width(), 100);
    }

    void testSelectRect()
    {
        Fixture f;
        QVERIFY(!f.image->globalSelection());

        LayerCommand select = command(LayerCommand::SelectRect);
        select.rect = QRect(10, 20, 30, 40);
        QString summary;
        QString error;
        QVERIFY2(f.run(select, &summary, &error), qPrintable(error));
        QVERIFY(f.image->globalSelection());
        QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(10, 20, 30, 40));
        QVERIFY(summary.contains("x=10 y=20 width=30 height=40"));

        // a second one replaces the first; the part outside of the image is cut off
        select.rect = QRect(80, 60, 50, 50);
        QVERIFY2(f.run(select, &summary, &error), qPrintable(error));
        QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(80, 60, 20, 20));

        // Edit > Undo: one step each
        f.undo();
        QVERIFY(f.image->globalSelection());
        QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(10, 20, 30, 40));
        f.undo();
        QVERIFY(!f.image->globalSelection() || f.image->globalSelection()->selectedExactRect().isEmpty());

        select.rect = QRect(500, 500, 10, 10); // outside
        QVERIFY(!f.run(select, nullptr, &error));
        QVERIFY(error.contains("outside"));
    }

    void testNoImage()
    {
        LayerOps ops{KisImageSP()};
        QString summary;
        QString error;
        QVERIFY(!ops.apply(command(LayerCommand::AddLayer), &summary, &error));
        QVERIFY(error.contains("no open document"));
    }
};

KISTEST_MAIN(LayerOpsTest)

#include "LayerOpsTest.moc"
