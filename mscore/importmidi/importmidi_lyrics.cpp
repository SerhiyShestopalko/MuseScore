#include "importmidi_lyrics.h"
#include "importmidi_inner.h"
#include "importmidi_fraction.h"
#include "importmidi_chord.h"
#include "importmidi_operations.h"
#include "libmscore/box.h"
#include "libmscore/element.h"
#include "libmscore/measurebase.h"
#include "libmscore/score.h"
#include "libmscore/staff.h"
#include "libmscore/text.h"
#include "midi/midifile.h"
#include "mscore/preferences.h"

#include <set>


namespace Ms {

extern Preferences preferences;

namespace MidiLyrics {

const std::string META_PREFIX = "@";
const std::string TEXT_PREFIX = "@T";


bool isMetaText(const std::string &text)
      {
      return text.substr(0, META_PREFIX.size()) == META_PREFIX;
      }

bool isLyricText(const std::string &text)
      {
      return !isMetaText(text) || text.substr(0, TEXT_PREFIX.size()) == TEXT_PREFIX;
      }

bool isLyricEvent(const MidiEvent &e)
      {
      return e.type() == ME_META && (e.metaType() == META_TEXT
                                     || e.metaType() == META_LYRIC);
      }

std::multimap<ReducedFraction, std::string>
extractLyricsFromTrack(const MidiTrack &track, int division, bool isDivisionInTps)
      {
      std::multimap<ReducedFraction, std::string> lyrics;

      for (const auto &i: track.events()) {
            const auto& e = i.second;
            if (isLyricEvent(e)) {
                  const uchar* data = (uchar*)e.edata();
                  std::string text = MidiCharset::fromUchar(data);
                  if (isLyricText(text)) {
                        const auto tick = toMuseScoreTicks(i.first, division, isDivisionInTps);
                                    // no charset handling here
                        lyrics.insert({tick, text});
                        }
                  }
            }
      return lyrics;
      }


#ifdef QT_DEBUG

bool areEqualIndexesSuccessive(const QList<MTrack> &tracks)
      {
      std::set<int> usedIndexes;

      int prevIndex = -1;
      for (const MTrack &track: tracks) {
            if (track.indexOfOperation < 0)
                  return false;
            if (track.indexOfOperation != prevIndex) {
                  const auto it = usedIndexes.find(track.indexOfOperation);
                  if (it == usedIndexes.end())
                        usedIndexes.insert(track.indexOfOperation);
                  else
                        return false;
                  prevIndex = track.indexOfOperation;
                  }
            }
      return true;
      }

#endif


struct BestTrack
      {
      int index = -1;
                  // <orig time, current time - after quantization>
      std::vector<std::pair<ReducedFraction, ReducedFraction>> matchedLyricTimes;
      };

// find track to insert lyrics
// it will be the track with max onTime match cases, except drum tracks

BestTrack findBestTrack(
            const QList<MTrack> &tracks,
            const std::multimap<ReducedFraction, std::string> &lyricTrack,
            const std::set<int> &usedTracks)
      {
      BestTrack bestTrack;
      int maxMatches = 0;

      Q_ASSERT_X(areEqualIndexesSuccessive(tracks),
                 "MidiLyrics::findBestTrack", "Equal indexes of operations are not successive");

      for (int i = 0; i != tracks.size(); ++i) {
            if (tracks[i].mtrack->drumTrack() || usedTracks.find(i) != usedTracks.end())
                  continue;
                        // due to staff split there can be equal indexes of operation
            if (i > 0 && tracks[i].indexOfOperation == tracks[i - 1].indexOfOperation)
                  continue;

            int matches = 0;
            for (const auto &chord: tracks[i].chords) {
                  for (const auto &note: chord.second.notes) {
                        if (lyricTrack.find(note.origOnTime) != lyricTrack.end()) {
                              ++matches;
                              bestTrack.matchedLyricTimes.push_back({chord.first,
                                                                     note.origOnTime});
                              break;
                              }
                        }
                  }
            if (matches > maxMatches) {
                  maxMatches = matches;
                  bestTrack.index = tracks[i].indexOfOperation;
                  }
            }

      return bestTrack;
      }

bool isTitlePrefix(const QString &text)
      {
      return (text.left(TEXT_PREFIX.size()) == QString::fromStdString(TEXT_PREFIX));
      }

void addTitleToScore(Score *score, const QString &string, int textCounter)
      {
      Text* text = new Text(score);
      if (textCounter == 1)
            text->setTextStyleType(TextStyleType::TITLE);
      else if (textCounter == 2)
            text->setTextStyleType(TextStyleType::COMPOSER);
      text->setPlainText(string.right(string.size() - TEXT_PREFIX.size()));

      MeasureBase* measure = score->first();
      if (measure->type() != Element::Type::VBOX) {
            measure = new VBox(score);
            measure->setTick(0);
            measure->setNext(score->first());
            score->measures()->add(measure);
            }
      measure->add(text);
      }

// remove slashes in kar format

QString removeSlashes(const QString &text)
      {
      QString newText = text;
      newText = newText.replace("/", "");
      newText = newText.replace("\\", "");
      return newText;
      }

std::string removeSlashes(const std::string &text)
      {
      std::string str = text;
      char chars[] = "/\\";
      for (unsigned int i = 0; i != strlen(chars); ++i)
            str.erase(std::remove(str.begin(), str.end(), chars[i]), str.end());
      return str;
      }

void addTitleIfAny(const std::multimap<ReducedFraction, std::string> &lyricTrack, Score *score)
      {
      int textCounter = 0;
      for (const auto &lyric: lyricTrack) {
            if (lyric.first == ReducedFraction(0, 1)) {
                  QString text = MidiCharset::convertToCharset(lyric.second);
                  if (isTitlePrefix(text)) {
                        ++textCounter;
                        addTitleToScore(score, text, textCounter);
                        }
                  }
            else if (lyric.first > ReducedFraction(0, 1)) {
                  break;
                  }
            }
      }

void addLyricsToScore(
            const std::multimap<ReducedFraction, std::string> &lyricTrack,
            const std::vector<std::pair<ReducedFraction, ReducedFraction>> &matchedLyricTimes,
            const Staff *staffAddTo)
      {
      Score *score = staffAddTo->score();

      addTitleIfAny(lyricTrack, score);

      for (const auto &timePair: matchedLyricTimes) {
            const auto quantizedTime = timePair.first;
            const auto originalTime = timePair.second;
            const auto it = lyricTrack.find(originalTime);

            Q_ASSERT_X(it != lyricTrack.end(),
                       "MidiLyrics::addLyricsToScore", "Lyric time not found");

            QString text = MidiCharset::convertToCharset(it->second);
            if (originalTime != ReducedFraction(0, 1) || !isTitlePrefix(text)) { // not title
                  score->addLyrics(quantizedTime.ticks(), staffAddTo->idx(),
                                   removeSlashes(text));
                  }
            }
      }

void extractLyricsToMidiData(const MidiFile *mf)
      {
      for (const auto &t: mf->tracks()) {
            const auto lyrics = extractLyricsFromTrack(t, mf->division(), mf->isDivisionInTps());
            if (!lyrics.empty())
                  preferences.midiImportOperations.data()->lyricTracks.push_back(lyrics);
            }
      }

void setInitialLyricsFromMidiData(const QList<MTrack> &tracks)
      {
      std::set<int> usedTracks;
      auto &data = *preferences.midiImportOperations.data();
      const auto &lyricTracks = data.lyricTracks;
      if (lyricTracks.isEmpty())
            return;

      for (int i = 0; i != lyricTracks.size(); ++i) {
            const BestTrack bestTrack = findBestTrack(tracks, lyricTracks[i], usedTracks);
            if (bestTrack.index >= 0) {
                  usedTracks.insert(bestTrack.index);
                  addLyricsToScore(lyricTracks[i],
                                   bestTrack.matchedLyricTimes,
                                   tracks[bestTrack.index].staff);
                  data.trackOpers.lyricTrackIndex.setValue(bestTrack.index, i);
                  }
            }
      }

std::vector<std::pair<ReducedFraction, ReducedFraction> > findMatchedLyricTimes(
            const std::multimap<ReducedFraction, MidiChord> &chords,
            const std::multimap<ReducedFraction, std::string> &lyricTrack)
      {
                  // <chord quantized on time, chord original on time>
      std::vector<std::pair<ReducedFraction, ReducedFraction> > matchedLyricTimes;

      for (const auto &chord: chords) {
            for (const auto &note: chord.second.notes) {
                  if (lyricTrack.find(note.origOnTime) != lyricTrack.end()) {
                        matchedLyricTimes.push_back({chord.first, note.origOnTime});
                        break;
                        }
                  }
            }
      return matchedLyricTimes;
      }

void setLyricsFromOperations(const QList<MTrack> &tracks)
      {
      const auto &lyricTracks = preferences.midiImportOperations.data()->lyricTracks;
      if (lyricTracks.isEmpty())
            return;
      for (const auto &track: tracks) {
            const auto &opers = preferences.midiImportOperations.data()->trackOpers;
            const int lyricTrackIndex = opers.lyricTrackIndex.value(track.indexOfOperation);
            if (lyricTrackIndex >= 0 && lyricTrackIndex < lyricTracks.size()) {
                  const auto &lyricTrack = lyricTracks[lyricTrackIndex];
                  const auto matchedLyricTimes = findMatchedLyricTimes(track.chords, lyricTrack);

                  addLyricsToScore(lyricTrack, matchedLyricTimes, track.staff);
                  }
            }
      }

void setLyricsToScore(QList<MTrack> &tracks)
      {
      const auto *data = preferences.midiImportOperations.data();
      if (data->processingsOfOpenedFile == 0) {
            setInitialLyricsFromMidiData(tracks);
            }
      else {
            setLyricsFromOperations(tracks);
            }
      }

QList<std::string> makeLyricsListForUI()
      {
      QList<std::string> list;
      const auto &lyrics = preferences.midiImportOperations.data()->lyricTracks;
      if (lyrics.isEmpty())
            return list;

      const unsigned int symbolLimit = 16;

      for (const auto &trackLyric: lyrics) {
            std::string lyricText;

            for (const auto &lyric: trackLyric) {
                  const auto &text = removeSlashes(lyric.second);
                  if (isMetaText(text))
                        continue;
                  if (!lyricText.empty())
                        lyricText += " ";       // visual text delimeter
                  if (lyricText.size() + text.size() > symbolLimit)
                        lyricText += text.substr(0, symbolLimit - lyricText.size());
                  else
                        lyricText += text;
                  if (lyricText.size() >= symbolLimit - 1) {
                        lyricText += "...";
                        break;
                        }
                  }
            list.push_back(lyricText);
            }
      return list;
      }

} // namespace MidiLyrics
} // namespace Ms
