// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// The artwork fills the window, while all controls stay in its safe content area.
struct GoldCinematicBackdrop: View {
    let game: ConsoleTitle?
    let theme: GoldTheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var artwork: Artwork?

    private struct Artwork {
        let url: URL
        let image: UIImage
    }

    private var candidates: [URL] {
        guard let game else { return [] }
        let paths = ["pic0.png", "pic1.png", "icon0.png"].compactMap { name -> URL? in
            guard let url = game.folder?.appendingPathComponent("sce_sys/" + name),
                  let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]),
                  values.isRegularFile == true, values.isSymbolicLink != true else { return nil }
            return url
        }
        if let fallback = GoldArtwork.url(game) { return paths + [fallback] }
        return paths
    }

    var body: some View {
        let paths = candidates
        GeometryReader { geometry in
            ZStack {
                LinearGradient(colors: [theme.colors[1], Color.black],
                               startPoint: .topTrailing, endPoint: .bottomLeading)
                if let artwork {
                    Image(uiImage: artwork.image).resizable().scaledToFill()
                        .frame(width: geometry.size.width, height: geometry.size.height)
                        .clipped().id(artwork.url).transition(.opacity)
                }
                LinearGradient(stops: [
                    .init(color: .black.opacity(0.76), location: 0),
                    .init(color: .black.opacity(0.43), location: 0.42),
                    .init(color: .black.opacity(0.26), location: 1)
                ], startPoint: .leading, endPoint: .trailing)
                LinearGradient(stops: [
                    .init(color: .black.opacity(0.13), location: 0),
                    .init(color: .clear, location: 0.46),
                    .init(color: .black.opacity(0.87), location: 1)
                ], startPoint: .top, endPoint: .bottom)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
            .clipped()
        }
        .ignoresSafeArea().allowsHitTesting(false).accessibilityHidden(true)
        .task(id: paths) {
            let next = await Task.detached(priority: .utility) { () -> Artwork? in
                for url in paths {
                    if let image = GoldArtwork.load(url, large: true) {
                        return Artwork(url: url, image: image)
                    }
                }
                return nil
            }.value
            guard !Task.isCancelled else { return }
            withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.28)) { artwork = next }
        }
    }
}

struct GoldCinematicHome: View {
    let entries: [GoldHomeItem]
    let selected: GoldHomeItem?
    let size: CGSize
    let accent: Color
    let showingLibrary: Bool
    let select: (GoldHomeItem) -> Void
    let activate: (GoldHomeItem) -> Void
    let details: (ConsoleTitle) -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @FocusState private var focusedItem: String?
    @State private var scrollTarget: String?

    private var games: [GoldHomeItem] { entries.filter { $0.game != nil } }
    private var sections: [GoldHomeItem] { entries.filter { $0.section != nil } }
    private var wide: Bool { size.width > size.height * 1.22 }
    private var selectedSide: CGFloat {
        min(wide ? 166 : 148, max(68, min(size.width * (wide ? 0.24 : 0.39),
                                          size.height * (wide ? 0.46 : 0.26))))
    }
    private var gap: CGFloat { min(17, max(8, size.width * 0.024)) }
    private var gridSide: CGFloat {
        let cols = max(2, Int((size.width + gap) / ((wide ? 88 : 74) + gap)))
        return max(1, (size.width - CGFloat(cols - 1) * gap) / CGFloat(cols))
    }

    var body: some View {
        Group {
            if showingLibrary { library }
            else { home }
        }
        .onChange(of: focusedItem) { _, id in
            if let id, let item = games.first(where: { $0.id == id }) { select(item) }
        }
        .accessibilityIdentifier(showingLibrary ? "cinematic-library" : "cinematic-home")
    }

    private var home: some View {
        ScrollView(.vertical) {
            if let selected, !games.isEmpty {
                VStack(alignment: .leading, spacing: wide ? 5 : 11) {
                    carousel
                    Text(selected.title).font(.subheadline.weight(.semibold))
                        .lineLimit(1).accessibilityHidden(true)
                    Text(selected.title).font(.system(size: wide ? min(40, max(21, size.height * 0.108)) : 34,
                                                      weight: .bold))
                        .lineLimit(2).minimumScaleFactor(0.72)
                        .accessibilityIdentifier("cinematic-title")
                    if let game = selected.game {
                        HStack(spacing: 8) {
                            GoldPlatformBadge(game: game)
                            Text(game.id)
                            if game.folder == nil { Text(L("jit.setup.files.missing")).lineLimit(1) }
                        }
                        .font(.subheadline).foregroundStyle(.white.opacity(0.75))
                        .accessibilityIdentifier("cinematic-title-metadata")
                    }
                    actions(selected).padding(.top, wide ? 7 : 12)
                }
                .frame(maxWidth: .infinity, minHeight: wide ? 0 : min(size.height * 0.50, 300),
                       alignment: .topLeading)
                .padding(.bottom, 4)
            } else {
                ContentUnavailableView(G("cinematic_empty", "No games"), systemImage: "gamecontroller")
                    .frame(maxWidth: .infinity, minHeight: min(size.height, 200))
            }
        }
        .scrollIndicators(.hidden)
    }

    private var carousel: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal) {
                LazyHStack(alignment: .top, spacing: gap) {
                    ForEach(games) { item in
                        let isSelected = selected?.id == item.id
                        let side = selectedSide * (isSelected ? 1 : 0.69)
                        tile(item, side: side, selected: isSelected, labelled: false)
                            .id(item.id)
                    }
                }
                .frame(height: selectedSide, alignment: .top)
                .scrollTargetLayout()
            }
            .scrollIndicators(.hidden)
            .scrollPosition(id: $scrollTarget, anchor: .leading)
            .onAppear {
                if let id = selected?.id, let index = games.firstIndex(where: { $0.id == id }),
                   CGFloat(index) * (selectedSide * 0.69 + gap) + selectedSide > size.width {
                    proxy.scrollTo(id, anchor: .leading)
                }
            }
            .onChange(of: focusedItem) { _, id in
                guard let id else { return }
                withAnimation(reduceMotion ? nil : .easeInOut(duration: 0.24)) {
                    proxy.scrollTo(id)
                }
            }
        }
        .accessibilityIdentifier("cinematic-carousel")
    }

    private var library: some View {
        ScrollView {
            let columns = max(2, Int((size.width + gap) / ((wide ? 88 : 74) + gap)))
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: gap, alignment: .top),
                                     count: columns), alignment: .leading, spacing: gap) {
                ForEach(games) { item in
                    tile(item, side: gridSide, selected: selected?.id == item.id, labelled: true)
                }
            }
            if !sections.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(sections) { item in
                        Button { activate(item) } label: {
                            HStack(spacing: 10) {
                                if let section = item.section {
                                    GoldSectionArtwork(section: section)
                                        .frame(width: 36, height: 36)
                                        .clipShape(RoundedRectangle(cornerRadius: 8))
                                }
                                Text(item.title).frame(maxWidth: .infinity, alignment: .leading)
                                Image(systemName: "chevron.right").foregroundStyle(accent)
                            }
                            .padding(8).contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("cinematic-tool-" + item.id)
                    }
                }
                .padding(.top, 12)
            }
            if let selected, selected.game != nil { actions(selected).padding(.top, 16) }
        }
        .scrollIndicators(.hidden)
        .defaultScrollAnchor(.top)
    }

    private func tile(_ item: GoldHomeItem, side: CGFloat, selected: Bool, labelled: Bool) -> some View {
        Button { select(item) } label: {
            VStack(alignment: .leading, spacing: 5) {
                if let game = item.game {
                    GoldCover(game: game, showUserStatus: false)
                        .frame(width: side, height: side)
                        .background(.white.opacity(0.08))
                        .clipShape(RoundedRectangle(cornerRadius: min(15, side * 0.13)))
                        .overlay {
                            RoundedRectangle(cornerRadius: min(15, side * 0.13))
                                .strokeBorder(.white.opacity(selected ? 1 : 0.16),
                                              lineWidth: selected ? 3 : 1)
                        }
                }
                if labelled {
                    Text(item.title).font(.footnote).lineLimit(1)
                        .foregroundStyle(.white.opacity(selected ? 1 : 0.74))
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .focused($focusedItem, equals: item.id)
        .accessibilityLabel(item.title)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .accessibilityIdentifier("cinematic-select-" + item.id)
        .contextMenu {
            if let game = item.game {
                Button(G("start_game", "Play"), systemImage: "play.fill") { select(item); activate(item) }
                Button(L("Сведения"), systemImage: "ellipsis") { select(item); details(game) }
            }
        }
    }

    private func actions(_ item: GoldHomeItem) -> some View {
        HStack(spacing: 10) {
            Button { activate(item) } label: {
                Label(G("start_game", "Play"), systemImage: "play.fill")
                    .font(.headline.weight(.semibold))
                    .padding(.horizontal, 27).frame(minHeight: 44)
                    .foregroundStyle(.black).background(.white, in: Capsule())
            }
            .buttonStyle(.plain).accessibilityIdentifier("cinematic-play")
            if let game = item.game {
                Button { details(game) } label: {
                    Image(systemName: "ellipsis").font(.headline.weight(.bold))
                        .frame(width: 44, height: 44)
                        .background(.white.opacity(0.19), in: Circle())
                }
                .buttonStyle(.plain).accessibilityLabel(L("Сведения"))
                .accessibilityIdentifier("cinematic-options")
            }
        }
    }
}
